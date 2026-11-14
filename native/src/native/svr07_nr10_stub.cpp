// RR: the one non-inline piece of the NR-10 probe stand-in (ufc_nr10_probe.h): the dump folder
// ufc_resolve_writeback.cpp's svr07_native_wb_dump and ufc_draw_log.cpp's texture dumps write to.

#include <string>

#include <windows.h>

#include <rex/cvar.h>

#include "ufc_nr10_probe.h"

std::string UfcNr10DumpDir() {
  static const std::string dir = [] {
    std::string d = rex::cvar::GetFlagByName("log_file");
    const size_t slash = d.find_last_of("\/");
    d = slash == std::string::npos ? std::string(".") : d.substr(0, slash);
    d += "\nr10_dump";
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
  }();
  return dir;
}
