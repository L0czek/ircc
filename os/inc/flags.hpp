#ifndef __FLAGS_HPP__
#define __FLAGS_HPP__

#include "cmsis_os2.h"
namespace os {

enum class OsWaitFlagsOpt {
  WaitAny = osFlagsWaitAny,
  WaitAll = osFlagsWaitAll,
  WaitClear = osFlagsNoClear
};

}

#endif
