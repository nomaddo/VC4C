/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "MemoryMappings.h"

#include "../GlobalValues.h"
#include "../Profiler.h"
#include "../intermediate/Helper.h"
#include "../intermediate/IntermediateInstruction.h"
#include "../intermediate/operators.h"
#include "../periphery/VPM.h"
#include "log.h"

using namespace vc4c;
using namespace vc4c::normalization;
using namespace vc4c::intermediate;
using namespace vc4c::operators;

using MappingCheck = MemoryInfo (*)(Method& method, const Local*, MemoryAccess&);

static MemoryInfo canLowerToRegisterReadOnly(Method& method, const Local* baseAddr, MemoryAccess& access);
static MemoryInfo canLowerToRegisterReadWrite(Method& method, const Local* baseAddr, MemoryAccess& access);
static MemoryInfo canLowerToPrivateVPMArea(Method& method, const Local* baseAddr, MemoryAccess& access);
static MemoryInfo canLowerToSharedVPMArea(Method& method, const Local* baseAddr, MemoryAccess& access);
static MemoryInfo canMapToTMUReadOnly(Method& method, const Local* baseAddr, MemoryAccess& access);
static MemoryInfo canMapToDMAReadWrite(Method& method, const Local* baseAddr, MemoryAccess& access);

static constexpr MappingCheck CHECKS[6] = {
    canLowerToRegisterReadOnly,  /* QPU_REGISTER_READONLY */
    canLowerToRegisterReadWrite, /* QPU_REGISTER_READWRITE */
    canLowerToPrivateVPMArea,    /* VPM_PER_QPU */
    canLowerToSharedVPMArea,     /* VPM_SHARED_ACCESS */
    canMapToTMUReadOnly,         /* RAM_LOAD_TMU */
    canMapToDMAReadWrite         /* RAM_READ_WRITE_VPM */
};

MemoryInfo normalization::checkMemoryMapping(Method& method, const Local* baseAddr, MemoryAccess& access)
{
    return CHECKS[static_cast<unsigned>(access.preferred)](method, baseAddr, access);
}

Optional<Value> normalization::getConstantValue(const Value& source)
{
    // can only read from constant global data, so the global is always the source
    const Global* global = source.local()->getBase(true)->as<Global>();
    if(auto literal = global->initialValue.getScalar())
        // scalar value
        return Value(*literal, global->initialValue.type.getElementType());
    if(global->initialValue.isZeroInitializer())
        // all entries are the same
        return Value::createZeroInitializer(global->initialValue.type.getElementType());
    if(global->initialValue.isUndefined())
        // all entries are undefined
        return Value(global->initialValue.type.getElementType());
    auto globalContainer = global->initialValue.getCompound();
    if(global->initialValue.isAllSame())
        // all entries are the same
        return globalContainer->at(0).toValue();
    if(globalContainer && source.local()->reference.second >= 0)
        // fixed index
        return globalContainer->at(static_cast<std::size_t>(source.local()->reference.second)).toValue();
    if(auto val = global->initialValue.toValue())
    {
        if(auto vector = val->checkVector())
        {
            return vector->isElementNumber() ? ELEMENT_NUMBER_REGISTER : NO_VALUE;
        }
    }
    return NO_VALUE;
}

/*
 * Tries to convert the array-type pointed to by the given local to a vector-type to fit into a single register.
 *
 * For this conversion to succeed, the array-element type must be a scalar of bit-width <= 32-bit and the size of the
 * array known to be less or equals to 16.
 */
static Optional<DataType> convertSmallArrayToRegister(const Local* local)
{
    // TODO can't we also lower e.g. uint4[4] into 1 register? Shouldn't be a problem if element access is implemented
    // correctly, since the anyway can use vloadN/vstoreN to access vectors of any size. Just need to make sure not to
    // set whole vector when storing first element!
    // TODO for gentype3/vload3/vstore3, need to make sure to either use padded or not-padded size for all access!
    const Local* base = local->getBase(true);
    if(auto ptrType = base->type.getPointerType())
    {
        const auto& baseType = ptrType->elementType;
        auto arrayType = baseType.getArrayType();
        if(arrayType && arrayType->size <= NATIVE_VECTOR_SIZE && arrayType->elementType.isScalarType())
            return arrayType->elementType.toVectorType(static_cast<uint8_t>(arrayType->size));
    }
    return {};
}

static bool isMemoryOnlyRead(const Local* local)
{
    auto base = local->getBase(true);
    if(base->is<Parameter>() && has_flag(base->as<const Parameter>()->decorations, ParameterDecorations::READ_ONLY))
        return true;

    if(base->is<Global>() && base->as<Global>()->isConstant)
        return true;

    if(base->type.getPointerType() && base->type.getPointerType()->addressSpace == AddressSpace::CONSTANT)
        return true;

    // TODO also check for no actual writes. Need to heed index-calculation from base!
    return false;
}

// Finds the next instruction writing the given value into memory
static NODISCARD InstructionWalker findNextValueStore(
    InstructionWalker it, const Value& src, std::size_t limit, const Local* sourceLocation)
{
    while(!it.isEndOfBlock() && limit > 0)
    {
        auto memInstr = it.get<MemoryInstruction>();
        if(memInstr != nullptr && memInstr->op == MemoryOperation::WRITE && memInstr->getSource() == src)
        {
            return it;
        }
        if(memInstr != nullptr && memInstr->getDestination().local()->getBase(true) == sourceLocation)
        {
            // there is some other instruction writing into the memory we read, it could have been changed -> abort
            // TODO can we be more precise and abort only if the same index is written?? How to determine??
            return it.getBasicBlock()->walkEnd();
        }
        if(it.get<MemoryBarrier>() || it.get<Branch>() || it.get<MutexLock>() || it.get<SemaphoreAdjustment>())
            break;
        it.nextInBlock();
        --limit;
    }
    return it.getBasicBlock()->walkEnd();
}

static NODISCARD std::pair<InstructionWalker, InstructionWalker> insert64BitWrite(Method& method, InstructionWalker it,
    Local* address, Value lower, Value upper, MemoryInstruction* origInstruction = nullptr)
{
    if(origInstruction->guardAccess)
    {
        it.emplace(new MutexLock(MutexAccess::LOCK));
        it.nextInBlock();
    }
    auto lowerIndex = Value(address, method.createPointerType(TYPE_INT32));
    it.emplace(
        new MemoryInstruction(MemoryOperation::WRITE, Value(lowerIndex), std::move(lower), Value(INT_ONE), false));
    if(origInstruction)
        it->copyExtrasFrom(origInstruction);
    auto startIt = it;
    it.nextInBlock();
    auto upperIndex = assign(it, lowerIndex.type) = lowerIndex + 4_val;
    if(auto ref = lowerIndex.local()->reference.first)
        upperIndex.local()->reference.first = ref;
    it.emplace(
        new MemoryInstruction(MemoryOperation::WRITE, std::move(upperIndex), std::move(upper), Value(INT_ONE), false));
    if(origInstruction)
        it->copyExtrasFrom(origInstruction);
    it.nextInBlock();
    if(origInstruction->guardAccess)
    {
        it.emplace(new MutexLock(MutexAccess::RELEASE));
        it.nextInBlock();
    }
    return std::make_pair(it, startIt);
}

std::pair<MemoryAccessMap, FastSet<InstructionWalker>> normalization::determineMemoryAccess(Method& method)
{
    // TODO lower local/private struct-elements into VPM?! At least for single structs
    CPPLOG_LAZY(logging::Level::DEBUG, log << "Determining memory access for kernel: " << method.name << logging::endl);
    MemoryAccessMap mapping;
    FastSet<InstructionWalker> allWalkers;
    for(const auto& param : method.parameters)
    {
        if(!param.type.getPointerType())
            continue;
        auto pointerType = param.type.getPointerType();
        if(pointerType->addressSpace == AddressSpace::CONSTANT)
        {
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Constant parameter '" << param.to_string() << "' will be read from RAM via TMU"
                    << logging::endl);
            mapping[&param].preferred = MemoryAccessType::RAM_LOAD_TMU;
            // fall-back, e.g. for memory copy
            mapping[&param].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
        }
        else if(pointerType->addressSpace == AddressSpace::GLOBAL)
        {
            if(isMemoryOnlyRead(&param))
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Global parameter '" << param.to_string()
                        << "' without any write access will be read from RAM via TMU" << logging::endl);
                mapping[&param].preferred = MemoryAccessType::RAM_LOAD_TMU;
                // fall-back, e.g. for memory copy
                mapping[&param].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
            }
            else
            {
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Global parameter '" << param.to_string()
                        << "' which is written to will be stored in RAM and accessed via VPM" << logging::endl);
                mapping[&param].preferred = MemoryAccessType::RAM_READ_WRITE_VPM;
                mapping[&param].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
            }
        }
        else if(pointerType->addressSpace == AddressSpace::LOCAL)
        {
            // TODO if last access index is known and fits into VPM, set for VPM-or-RAM
            // TODO need to make sure the correct size is used (by default, lowering to VPM uses the pointed-to-type
            // which could be wrong!)
            CPPLOG_LAZY(logging::Level::DEBUG,
                log << "Local parameter '" << param.to_string() << "' will be stored in RAM and accessed via VPM"
                    << logging::endl);
            mapping[&param].preferred = MemoryAccessType::RAM_READ_WRITE_VPM;
            mapping[&param].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
        }
        else
            throw CompilationError(
                CompilationStep::NORMALIZER, "Invalid address space for pointer parameter", param.to_string(true));
    }

    InstructionWalker it = method.walkAllInstructions();
    // The 64 bit store instructions which need to be split up in two 32 bit store instructions and the locals for the
    // lower and upper words
    FastMap<const MemoryInstruction*, std::pair<const Local*, const Local*>> rewrite64BitStoresTo32;
    while(!it.isEndOfMethod())
    {
        if(auto memInstr = it.get<MemoryInstruction>())
        {
            auto rewriteParts = rewrite64BitStoresTo32.find(memInstr);
            if(rewriteParts != rewrite64BitStoresTo32.end())
            {
                // rewrite the 64-bit store to 2 32-bit stores
                InstructionWalker startIt;
                std::tie(it, startIt) = insert64BitWrite(method, it, memInstr->getDestination().local(),
                    rewriteParts->second.first->createReference(), rewriteParts->second.second->createReference(),
                    memInstr);
                rewrite64BitStoresTo32.erase(rewriteParts);
                it.erase();

                // continue with exactly the first store instruction
                memInstr = startIt.get<MemoryInstruction>();
                it = startIt;
            }
            else if(memInstr->op == MemoryOperation::WRITE && !memInstr->hasConditionalExecution() &&
                memInstr->getSource().type.getElementType() == TYPE_INT64)
            {
                // we can lower some more 64-bit writes, if the values can either be written statically (e.g. a
                // constant) or it is guaranteed that the upper word is zero (e.g. converted from 32-bit word).
                auto source = intermediate::getSourceValue(memInstr->getSource());
                // NOTE: we only check for exactly 32-bit, since otherwise VPM would only write the actually used bits,
                // not all 32
                auto lowerSource = source.type.getScalarBitCount() == 32 ? source : source.getConstantValue();
                if(lowerSource)
                {
                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Converting write of 64-bit '" << memInstr->getSource().to_string()
                            << "' where the upper part is guaranteed to be zero to a write of the lower part and zero"
                            << logging::endl);

                    // fix type of constant
                    if(lowerSource->type == TYPE_INT64)
                        lowerSource->type = TYPE_INT32;

                    // rewrite the 64-bit store to 2 32-bit stores
                    InstructionWalker startIt;
                    std::tie(it, startIt) = insert64BitWrite(method, it, memInstr->getDestination().local(),
                        *lowerSource, Value(Literal(0u), TYPE_INT32), memInstr);
                    it.erase();

                    // continue with exactly the first store instruction
                    memInstr = startIt.get<MemoryInstruction>();
                    it = startIt;
                }
            }
            else if(memInstr->op == MemoryOperation::READ && !memInstr->hasConditionalExecution() &&
                memInstr->getDestination().local()->getUsers(LocalUse::Type::READER).size() == 1)
            {
                // convert read-then-write to copy
                auto nextIt = findNextValueStore(
                    it, memInstr->getDestination(), 16 /* TODO */, memInstr->getSource().local()->getBase(true));
                auto nextMemInstr = nextIt.isEndOfBlock() ? nullptr : nextIt.get<const MemoryInstruction>();
                if(nextMemInstr != nullptr && !nextIt->hasConditionalExecution() &&
                    nextMemInstr->op == MemoryOperation::WRITE &&
                    nextMemInstr->getSource().getSingleWriter() == memInstr &&
                    nextMemInstr->getSourceElementType().getInMemoryWidth() ==
                        memInstr->getDestinationElementType().getInMemoryWidth())
                {
                    LCOV_EXCL_START
                    CPPLOG_LAZY_BLOCK(
                        logging::Level::DEBUG, {
                            logging::debug()
                                << "Found reading of memory where the sole usage writes the value back into memory: "
                                << memInstr->to_string() << logging::endl;
                            logging::debug()
                                << "Replacing manual copy of memory with memory copy instruction for write: "
                                << nextMemInstr->to_string() << logging::endl;
                        });
                    LCOV_EXCL_STOP

                    auto src = memInstr->getSource();
                    auto dest = nextMemInstr->getDestination();

                    if(nextMemInstr->getSourceElementType().getInMemoryWidth() !=
                            nextMemInstr->getDestinationElementType().getInMemoryWidth() ||
                        memInstr->getSourceElementType().getInMemoryWidth() !=
                            memInstr->getDestinationElementType().getInMemoryWidth())
                    {
                        // we read/write a different data type than the pointer type, this can e.g. happen when
                        // combining vloadN/vstoreN into single copy, where the actual types are vectors, but the
                        // pointers are to scalars.
                        // -> need to find the actual type by using the element types of the in-register type
                        auto valueType = method.createPointerType(nextMemInstr->getSourceElementType());
                        {
                            auto tmp = method.addNewLocal(valueType);
                            if(auto loc = src.checkLocal())
                                tmp.local()->reference = loc->reference;
                            assign(nextIt, tmp) = src;
                            src = tmp;
                        }
                        {
                            auto tmp = method.addNewLocal(valueType);
                            if(auto loc = dest.checkLocal())
                                tmp.local()->reference = loc->reference;
                            assign(nextIt, tmp) = dest;
                            dest = tmp;
                        }
                    }
                    it.erase();
                    nextIt.reset(new MemoryInstruction(MemoryOperation::COPY, Value(std::move(dest)), std::move(src),
                        Value(nextMemInstr->getNumEntries()), nextMemInstr->guardAccess));
                    // continue with the next instruction after the read in the next iteration
                    continue;
                }
            }
            // fall-through on purpose, since the below block also handles cases which match the first condition of the
            // above block, but not the second (e.g. read and write of 64-bit struct in different blocks)
            if(memInstr->op == MemoryOperation::READ && !memInstr->hasConditionalExecution() &&
                memInstr->getDestination().type.getElementType() == TYPE_INT64 && memInstr->getSource().checkLocal() &&
                memInstr->getSource().local()->getBase(true)->type.getElementType().getStructType())
            {
                // This is a "read of a 64-bit integer value from a memory area which refers to a struct pointer"
                CPPLOG_LAZY(logging::Level::DEBUG,
                    log << "Found reading of 64-bit integer bit-cast from struct pointer: " << memInstr->to_string()
                        << logging::endl);
                bool canBeSplitUp = true;
                FastSet<const MoveOperation*> movesToRewrite;
                FastSet<const Operation*> shiftsToRewrite;
                FastSet<const MemoryInstruction*> storesToRewrite;

                // Check whether all uses are used in a supported way (e.g. actually only access of the lower 32 bit or
                // copy the whole value)
                for(auto user : memInstr->getDestination().local()->getUsers(LocalUse::Type::READER))
                {
                    if(auto move = dynamic_cast<const MoveOperation*>(user))
                    {
                        // check for truncation from 64 to 32 bits
                        if(move->getOutput()->type.getScalarBitCount() != 32)
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Unsupported move, aborting rewrite: " << move->to_string() << logging::endl);
                            canBeSplitUp = false;
                            break;
                        }
                        // moves which truncate the 64 bit integer to 32 bit are accepted
                        movesToRewrite.emplace(move);
                    }
                    else if(auto memAccess = dynamic_cast<const MemoryInstruction*>(user))
                    {
                        // memory writes accesses using this value are accepted
                        if(memAccess->op != MemoryOperation::WRITE)
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Unsupported memory access, aborting rewrite: " << memAccess->to_string()
                                    << logging::endl);
                            canBeSplitUp = false;
                            break;
                        }
                        storesToRewrite.emplace(memAccess);
                    }
                    else if(auto op = dynamic_cast<const Operation*>(user))
                    {
                        if(op->op == OP_SHR && op->getSecondArg())
                        {
                            auto shiftOffset = op->assertArgument(1).getLiteralValue();
                            if(auto writer = op->assertArgument(1).getSingleWriter())
                            {
                                auto precalc = writer->precalculate();
                                shiftOffset = precalc.first ? precalc.first->getLiteralValue() : shiftOffset;
                            }

                            if(shiftOffset == 32_lit)
                            {
                                // shift exactly 1 word -> reads upper word
                                shiftsToRewrite.emplace(op);
                            }
                            else
                            {
                                CPPLOG_LAZY(logging::Level::DEBUG,
                                    log << "Unsupported shift of 64 bit integer, aborting rewrite: "
                                        << user->to_string() << logging::endl);
                                canBeSplitUp = false;
                            }
                        }
                        else
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Unsupported operation with 64 bit integer, aborting rewrite: "
                                    << user->to_string() << logging::endl);
                            canBeSplitUp = false;
                        }
                    }
                    else
                    {
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Unsupported access to 64 bit integer, aborting rewrite: " << user->to_string()
                                << logging::endl);
                        canBeSplitUp = false;
                    }
                }
                if(canBeSplitUp)
                {
                    // split load into 2 loads (upper and lower word), mark stores for conversion
                    auto origLocal = memInstr->getDestination();
                    auto lowerLocal = method.addNewLocal(TYPE_INT32, origLocal.local()->name + ".lower");
                    auto upperLocal = method.addNewLocal(TYPE_INT32, origLocal.local()->name + ".upper");

                    CPPLOG_LAZY(logging::Level::DEBUG,
                        log << "Splitting '" << origLocal.to_string() << "' into '" << lowerLocal.to_string()
                            << "' and '" << upperLocal.to_string() << '\'' << logging::endl);

                    if(memInstr->guardAccess)
                    {
                        it.emplace(new MutexLock(MutexAccess::LOCK));
                        it.nextInBlock();
                    }
                    auto lowerIndex = Value(memInstr->getSource().local(), method.createPointerType(TYPE_INT32));
                    it.emplace(new MemoryInstruction(
                        MemoryOperation::READ, Value(lowerLocal), Value(lowerIndex), Value(INT_ONE), false));
                    it->copyExtrasFrom(memInstr);
                    auto startIt = it;
                    it.nextInBlock();
                    auto upperIndex = assign(it, lowerIndex.type) = lowerIndex + 4_val;
                    if(auto ref = lowerIndex.local()->reference.first)
                        upperIndex.local()->reference.first = ref;
                    it.emplace(new MemoryInstruction(
                        MemoryOperation::READ, Value(upperLocal), std::move(upperIndex), Value(INT_ONE), false));
                    it->copyExtrasFrom(memInstr);
                    it.nextInBlock();
                    if(memInstr->guardAccess)
                    {
                        it.emplace(new MutexLock(MutexAccess::RELEASE));
                        it.nextInBlock();
                    }

                    for(auto store : storesToRewrite)
                    {
                        // mark all stores for rewrite
                        rewrite64BitStoresTo32.emplace(store,
                            std::pair<const Local*, const Local*>{lowerLocal.checkLocal(), upperLocal.checkLocal()});
                    }

                    for(auto move : movesToRewrite)
                        // replace all truncations with usage of lower word
                        const_cast<MoveOperation*>(move)->replaceValue(origLocal, lowerLocal, LocalUse::Type::READER);

                    for(auto shift : shiftsToRewrite)
                    {
                        // replace all shifts of the upper word to lower with upper word
                        // use a shift by zero, so we do not need to access the instruction walker
                        const_cast<Operation*>(shift)->setArgument(0, upperLocal);
                        const_cast<Operation*>(shift)->setArgument(1, 0_val);
                    }

                    it.erase();

                    // continue with exactly the first load instruction
                    memInstr = startIt.get<MemoryInstruction>();
                    it = startIt;
                }
            }
            for(const auto local : memInstr->getMemoryAreas())
            {
                if(mapping.find(local) != mapping.end())
                {
                    // local was already processed
                    mapping[local].accessInstructions.emplace(it);
                    continue;
                }
                mapping[local].accessInstructions.emplace(it);
                if(local->is<StackAllocation>())
                {
                    if(local->type.isSimpleType() || convertSmallArrayToRegister(local))
                    {
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Small stack value '" << local->to_string() << "' will be stored in a register"
                                << logging::endl);
                        mapping[local].preferred = MemoryAccessType::QPU_REGISTER_READWRITE;
                        // we cannot pack an array into a VPM cache line, since always all 16 elements are read/written
                        // and we would overwrite the other elements
                        mapping[local].fallback = local->type.isSimpleType() ? MemoryAccessType::VPM_PER_QPU :
                                                                               MemoryAccessType::RAM_READ_WRITE_VPM;
                    }
                    else if(!local->type.getElementType().getStructType())
                    {
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Stack value '" << local->to_string()
                                << "' will be stored in VPM per QPU (with fall-back to RAM via VPM)" << logging::endl);
                        mapping[local].preferred = MemoryAccessType::VPM_PER_QPU;
                        mapping[local].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
                    }
                    else
                    {
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Struct stack value '" << local->to_string()
                                << "' will be stored in RAM per QPU (via VPM)" << logging::endl);
                        mapping[local].preferred = MemoryAccessType::RAM_READ_WRITE_VPM;
                        mapping[local].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
                    }
                }
                else if(local->is<Global>())
                {
                    if(isMemoryOnlyRead(local))
                    {
                        // global buffer
                        if(getConstantValue(memInstr->getSource()))
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Constant element of constant buffer '" << local->to_string()
                                    << "' will be stored in a register " << logging::endl);
                            mapping[local].preferred = MemoryAccessType::QPU_REGISTER_READONLY;
                            mapping[local].fallback = MemoryAccessType::RAM_LOAD_TMU;
                        }
                        else if(convertSmallArrayToRegister(local))
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Small constant buffer '" << local->to_string()
                                    << "' will be stored in a register" << logging::endl);
                            mapping[local].preferred = MemoryAccessType::QPU_REGISTER_READONLY;
                            mapping[local].fallback = MemoryAccessType::RAM_LOAD_TMU;
                        }
                        else
                        {
                            CPPLOG_LAZY(logging::Level::DEBUG,
                                log << "Constant buffer '" << local->to_string() << "' will be read from RAM via TMU"
                                    << logging::endl);
                            mapping[local].preferred = MemoryAccessType::RAM_LOAD_TMU;
                            // fall-back, e.g. for memory copy
                            mapping[local].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
                        }
                    }
                    else if(!local->type.getElementType().getStructType())
                    {
                        // local buffer
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Local buffer '" << local->to_string()
                                << "' will be stored in VPM (with fall-back to RAM via VPM)" << logging::endl);
                        mapping[local].preferred = MemoryAccessType::VPM_SHARED_ACCESS;
                        mapping[local].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
                    }
                    else
                    {
                        // local buffer
                        CPPLOG_LAZY(logging::Level::DEBUG,
                            log << "Local struct '" << local->to_string() << "' will be stored in RAM via VPM"
                                << logging::endl);
                        mapping[local].preferred = MemoryAccessType::RAM_READ_WRITE_VPM;
                        mapping[local].fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
                    }
                }
                else
                {
                    bool allWritersArePhiNodes = true;
                    FastSet<const Local*> phiSources;
                    local->forUsers(LocalUse::Type::WRITER, [&](const LocalUser* writer) {
                        // skip the (possible write) we are currently processing
                        if(it.get() == writer)
                            return;
                        if(writer->hasDecoration(InstructionDecorations::PHI_NODE))
                        {
                            if(auto loc = writer->assertArgument(0).checkLocal())
                                phiSources.emplace(loc->getBase(true));
                            else
                                allWritersArePhiNodes = false;
                        }
                        else
                            allWritersArePhiNodes = false;
                    });
                    if(allWritersArePhiNodes)
                    {
                        // if the local is a PHI result from different memory addresses, we can still map it, if the
                        // source addresses have the same access type

                        // TODO can use TMU if all sources support it?! Could also use VPM, when dynamically selecting
                        // VPM base address

                        // add the instruction to all actual memory locations and update access types
                        for(const auto source : phiSources)
                        {
                            auto& map = mapping[source];
                            map.accessInstructions.emplace(it);
                            map.preferred = MemoryAccessType::RAM_READ_WRITE_VPM;
                            map.fallback = MemoryAccessType::RAM_READ_WRITE_VPM;
                        }

                        // delete the original (wrong) mapping, since it is not a proper memory area
                        mapping.erase(local);
                        CPPLOG_LAZY_BLOCK(logging::Level::DEBUG, {
                            auto& log = logging::debug()
                                << "Phi-node local '" << local->to_string()
                                << "' will not be mapped, these source locals will be mapped instead: ";
                            for(auto loc : phiSources)
                                log << loc->to_string() << ", ";
                            log << logging::endl;
                        });

                        // TODO this passes, but it will fail when doing the actual mapping, since it only checks the
                        // direct base of the source/destination, which is the phi-node output local, which is not
                        // mapped correctly!
                        throw CompilationError(CompilationStep::NORMALIZER,
                            "Accessing memory through a phi-node is not implemented yet", local->to_string(true));
                    }
                    else
                        // parameters MUST be handled before and there is no other type of memory objects
                        throw CompilationError(
                            CompilationStep::NORMALIZER, "Invalid local type for memory area", local->to_string(true));
                }
            }
            if(it.has())
                allWalkers.emplace(it);
        }
        it.nextInMethod();
    }

    return std::make_pair(std::move(mapping), std::move(allWalkers));
}

static MemoryInfo canLowerToRegisterReadOnly(Method& method, const Local* baseAddr, MemoryAccess& access)
{
    // a) the global is a constant scalar/vector which fits into a single register
    if(auto constant = getConstantValue(baseAddr->createReference()))
    {
        return MemoryInfo{baseAddr, MemoryAccessType::QPU_REGISTER_READONLY, nullptr, {}, constant};
    }
    // b) the global in a constant array small enough to be rewritten to fit into a single register (e.g. int[8])
    if(auto convertedType = convertSmallArrayToRegister(baseAddr))
    {
        // convert int[8] to int8
        if(auto convertedValue = baseAddr->as<Global>()->initialValue.toValue())
        {
            convertedValue->type = *convertedType;
            return MemoryInfo{
                baseAddr, MemoryAccessType::QPU_REGISTER_READONLY, nullptr, {}, convertedValue, convertedType};
        }
    }
    // c) the global is a constant where all accesses have constant indices and therefore all accessed elements can be
    // determined at compile time
    if(std::all_of(
           access.accessInstructions.begin(), access.accessInstructions.end(), [&](InstructionWalker it) -> bool {
               return getConstantValue(it.get<const intermediate::MemoryInstruction>()->getSource()).has_value();
           }))
        return MemoryInfo{baseAddr, MemoryAccessType::QPU_REGISTER_READONLY};

    // cannot lower to constant register, use fall-back
    access.preferred = access.fallback;
    return checkMemoryMapping(method, baseAddr, access);
}

static MemoryInfo canLowerToRegisterReadWrite(Method& method, const Local* baseAddr, MemoryAccess& access)
{
    // a) the private memory fits into a single register
    if(baseAddr->type.isScalarType())
    {
        if(auto stackAllocation = baseAddr->as<StackAllocation>())
            const_cast<StackAllocation*>(stackAllocation)->isLowered = true;
        return MemoryInfo{baseAddr, MemoryAccessType::QPU_REGISTER_READWRITE, nullptr, {},
            method.addNewLocal(baseAddr->type, "%lowered_stack")};
    }
    // b) the private memory is small enough to be rewritten to fit into a single register (e.g. int[4])
    auto convertedType = convertSmallArrayToRegister(baseAddr);
    if(convertedType)
    {
        if(auto stackAllocation = baseAddr->as<StackAllocation>())
            const_cast<StackAllocation*>(stackAllocation)->isLowered = true;
        return MemoryInfo{baseAddr, MemoryAccessType::QPU_REGISTER_READWRITE, nullptr, {},
            method.addNewLocal(*convertedType, "%lowered_stack"), convertedType};
    }

    // cannot lower to register, use fall-back
    access.preferred = access.fallback;
    return checkMemoryMapping(method, baseAddr, access);
}

static MemoryInfo canLowerToPrivateVPMArea(Method& method, const Local* baseAddr, MemoryAccess& access)
{
    // FIXME enable once storing with element (non-vector) offset into VPM is implemented
    // Retest: OpenCL-CTS/vload_private, OpenCL_CTS/vstore_private, emulate-memory
    auto area = static_cast<periphery::VPMArea*>(nullptr);
    // TODO method.vpm->addArea(baseAddr, baseAddr->type.getElementType(), true, method.metaData.getWorkGroupSize());
    if(area)
    {
        // mark stack allocation as lowered to VPM to skip reserving a stack area
        if(auto stackAllocation = baseAddr->as<StackAllocation>())
            const_cast<StackAllocation*>(stackAllocation)->isLowered = true;
        return MemoryInfo{
            baseAddr, MemoryAccessType::VPM_PER_QPU, area, {}, NO_VALUE, convertSmallArrayToRegister(baseAddr)};
    }

    // cannot lower to register, use fall-back
    access.preferred = access.fallback;
    return checkMemoryMapping(method, baseAddr, access);
}

static MemoryInfo canLowerToSharedVPMArea(Method& method, const Local* baseAddr, MemoryAccess& access)
{
    auto area = method.vpm->addArea(baseAddr, baseAddr->type.getElementType(), false);
    if(area)
        return MemoryInfo{
            baseAddr, MemoryAccessType::VPM_SHARED_ACCESS, area, {}, NO_VALUE, convertSmallArrayToRegister(baseAddr)};

    // cannot lower to register, use fall-back
    access.preferred = access.fallback;
    return checkMemoryMapping(method, baseAddr, access);
}

static MemoryInfo canMapToTMUReadOnly(Method& method, const Local* baseAddr, MemoryAccess& access)
{
    // TODO for better performance, the TMU flag should alternate in according to the order of usage (first read use
    // TMU0, second read use TMU1, ...)
    static thread_local bool tmuFlag = true;
    tmuFlag = !tmuFlag;
    return MemoryInfo{baseAddr, MemoryAccessType::RAM_LOAD_TMU, nullptr, {}, {}, {}, tmuFlag};
}

static const periphery::VPMArea* checkCacheMemoryAccessRanges(
    Method& method, const Local* baseAddr, FastAccessList<MemoryAccessRange>& accesRanges);

static MemoryInfo canMapToDMAReadWrite(Method& method, const Local* baseAddr, MemoryAccess& access)
{
    PROFILE_START(DetermineAccessRanges);
    auto ranges = analysis::determineAccessRanges(method, baseAddr, access);
    PROFILE_END(DetermineAccessRanges);

    if(!ranges.empty() && baseAddr->type.getPointerType() &&
        baseAddr->type.getPointerType()->addressSpace == AddressSpace::LOCAL)
    {
        auto area = checkCacheMemoryAccessRanges(method, baseAddr, ranges);
        if(area)
        {
            // for local/private memory, there is no need for initial load/write-back
            return MemoryInfo{baseAddr, MemoryAccessType::VPM_SHARED_ACCESS, area, std::move(ranges)};
        }
    }
    return MemoryInfo{baseAddr, MemoryAccessType::RAM_READ_WRITE_VPM};
}

static std::pair<bool, analysis::IntegerRange> checkWorkGroupUniformParts(
    FastAccessList<MemoryAccessRange>& accessRanges)
{
    analysis::IntegerRange offsetRange{std::numeric_limits<int>::max(), std::numeric_limits<int>::min()};
    const auto& firstUniformAddresses = accessRanges.front().groupUniformAddressParts;
    FastMap<Value, InstructionDecorations> differingUniformParts;
    bool allUniformPartsEqual = true;
    for(auto& entry : accessRanges)
    {
        if(entry.groupUniformAddressParts != firstUniformAddresses)
        {
            allUniformPartsEqual = false;
            for(const auto& pair : entry.groupUniformAddressParts)
            {
                if(firstUniformAddresses.find(pair.first) == firstUniformAddresses.end())
                    differingUniformParts.emplace(pair);
            }
            for(const auto& pair : firstUniformAddresses)
                if(entry.groupUniformAddressParts.find(pair.first) == entry.groupUniformAddressParts.end())
                    differingUniformParts.emplace(pair);
        }
        offsetRange.minValue = std::min(offsetRange.minValue, entry.offsetRange.minValue);
        offsetRange.maxValue = std::max(offsetRange.maxValue, entry.offsetRange.maxValue);
    }
    if(!allUniformPartsEqual)
    {
        if(std::all_of(differingUniformParts.begin(), differingUniformParts.end(),
               [](const std::pair<Value, InstructionDecorations>& part) -> bool {
                   return part.first.getLiteralValue().has_value();
               }))
        {
            // all work-group uniform values which differ between various accesses of the same local are literal
            // values. We can use this knowledge to still allow caching the local, by converting the literals to
            // dynamic offsets
            for(auto& entry : accessRanges)
            {
                auto it = entry.groupUniformAddressParts.begin();
                while(it != entry.groupUniformAddressParts.end())
                {
                    if(differingUniformParts.find(it->first) != differingUniformParts.end())
                    {
                        entry.offsetRange.minValue += it->first.getLiteralValue()->signedInt();
                        entry.offsetRange.maxValue += it->first.getLiteralValue()->signedInt();
                        entry.dynamicAddressParts.emplace(*it);
                        it = entry.groupUniformAddressParts.erase(it);
                    }
                    else
                        ++it;
                }
            }
            return checkWorkGroupUniformParts(accessRanges);
        }
        else
            return std::make_pair(false, analysis::IntegerRange{});
    }
    return std::make_pair(true, offsetRange);
}

static const periphery::VPMArea* checkCacheMemoryAccessRanges(
    Method& method, const Local* baseAddr, FastAccessList<MemoryAccessRange>& memoryAccessRanges)
{
    auto maxNumVectors = method.vpm->getMaxCacheVectors(TYPE_INT32, true);
    GroupedAccessRanges result;

    bool allUniformPartsEqual;
    analysis::IntegerRange offsetRange;
    std::tie(allUniformPartsEqual, offsetRange) = checkWorkGroupUniformParts(memoryAccessRanges);
    if(!allUniformPartsEqual)
    {
        LCOV_EXCL_START
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Cannot cache memory location " << baseAddr->to_string()
                << " in VPM, since the work-group uniform parts of the address calculations differ, which "
                   "is not yet supported!"
                << logging::endl);
        LCOV_EXCL_STOP
        return nullptr;
    }
    if((offsetRange.maxValue - offsetRange.minValue) >= maxNumVectors || (offsetRange.maxValue < offsetRange.minValue))
    {
        // this also checks for any over/underflow when converting the range to unsigned int in the next steps
        LCOV_EXCL_START
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Cannot cache memory location " << baseAddr->to_string()
                << " in VPM, the accessed range is too big: [" << offsetRange.minValue << ", " << offsetRange.maxValue
                << "]" << logging::endl);
        LCOV_EXCL_STOP
        return nullptr;
    }
    CPPLOG_LAZY(logging::Level::DEBUG,
        log << "Memory location " << baseAddr->to_string() << " is accessed via DMA in the dynamic range ["
            << offsetRange.minValue << ", " << offsetRange.maxValue << "]" << logging::endl);

    // TODO correct type?? Shouldn't it be baseAddr->type.getElmentType().toArrayType(...??
    auto accessedType = method.createArrayType(baseAddr->type,
        static_cast<unsigned>(offsetRange.maxValue - offsetRange.minValue + 1 /* bounds of range are inclusive! */));

    // XXX the local is not correct, at least not if there is a work-group uniform offset, but since all work-items
    // use the same work-group offset, it doesn't matter
    auto vpmArea = method.vpm->addArea(baseAddr, accessedType, false);
    if(vpmArea == nullptr)
    {
        CPPLOG_LAZY(logging::Level::DEBUG,
            log << "Memory location " << baseAddr->to_string() << " with dynamic access range [" << offsetRange.minValue
                << ", " << offsetRange.maxValue << "] cannot be cached in VPM, since it does not fit" << logging::endl);
        return nullptr;
    }
    return vpmArea;
}
