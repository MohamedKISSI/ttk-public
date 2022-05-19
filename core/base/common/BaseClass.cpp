#include <BaseClass.h>

#ifdef TTK_ENABLE_OPENMP
COMMON_EXPORTS int ttk::globalThreadNumber_ = omp_get_max_threads();
#else
COMMON_EXPORTS int ttk::globalThreadNumber_ = 1;
#endif

COMMON_EXPORTS int ttk::MPIrank_ = -1;
COMMON_EXPORTS int ttk::MPIsize_ = -1;

using namespace ttk;

BaseClass::BaseClass() : lastObject_{false}, wrapper_{nullptr} {
  threadNumber_ = ttk::globalThreadNumber_;

#ifdef TTK_ENABLE_MPI
  if(ttk::MPIrank_ == -1 && isRunningWithMPI()) {
    MPI_Comm_rank(MPI_COMM_WORLD, &ttk::MPIrank_);
    MPI_Comm_size(MPI_COMM_WORLD, &ttk::MPIsize_);
  }
#endif // TTK_ENABLE_MPI
}

int BaseClass::setWrapper(const Wrapper *wrapper) {
  wrapper_ = const_cast<Wrapper *>(wrapper);

  // dynamic_cast is impossible because the Wrapper class is incomplete
  setThreadNumber(reinterpret_cast<BaseClass *>(wrapper_)->threadNumber_);
  return 0;
}
