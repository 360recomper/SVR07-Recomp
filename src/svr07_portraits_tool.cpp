// svr07_portraits - builds the portrait-merged menu pacs without starting the game.
//
//   svr07_portraits <game assets folder> <mods folder> <output folder>
//
// Writes <output>\pac\menu\menuHD.pac / menuSD.pac and, when stale, plist360.arc /
// plist360_4x3.arc: the same files the game builds at startup.

#include <cstdio>

#include <rex/logging.h>

#include "svr07_portraits.h"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: svr07_portraits <game assets> <mods> <output>\n");
    return 2;
  }
  rex::InitLogging();
  svr07::BuildPortraitPacs(argv[1], argv[2], argv[3]);
  svr07::FixPacIndexes(argv[1], argv[2], argv[3]);
  return 0;
}
