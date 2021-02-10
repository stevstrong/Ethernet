#pragma once


//#define _DEBUG 1


#ifdef _DEBUG

  extern char dbg[];
  #define PRINTF(...) { sprintf(dbg, __VA_ARGS__); Serial.print(dbg); }

#else

  #define PRINTF(...)

#endif

