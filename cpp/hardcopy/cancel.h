#ifndef HARDCOPY_CANCEL_H_
#define HARDCOPY_CANCEL_H_

namespace hardcopy {

bool jobCancelled(const char* jobId);

bool currentJobCancelled();

}

#endif
