//
// Created by nomaddo on 18/06/02.
//

#ifndef VC4C_SCHEDULER_H
#define VC4C_SCHEDULER_H

#include "../Method.h"

namespace vc4c::optimizations {
    class Scheduler {

    public:
        Scheduler() = delete;
        static void doScheduling(Method & method, const Configuration & config);
    };
}

#endif //VC4C_SCHEDULER_H
