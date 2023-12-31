/*
    Implementation of GPTData class derivative with popt-based command
    line processing
    Copyright (C) 2010-2022 Roderick W. Smith

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*/

#ifndef __GPTCL_H
#define __GPTCL_H

#include "gpt.h"
#include <popt.h>
#include <map>

class GPTDataCL : public GPTData {
   protected:
      // Following are variables associated with popt parameters....
      char *attributeOperation, *backupFile, *partName, *hybrids;
      char *newPartInfo, *mbrParts, *twoParts, *outDevice, *typeCode;
      char *partGUID, *diskGUID;
      int alignment, deletePartNum, infoPartNum, largestPartNum, bsdPartNum;
      bool alignEnd;
      uint32_t tableSize;
      poptContext poptCon;
      std::map<int, char> typeRaw;

      int BuildMBR(char* argument, int isHybrid);
   public:
      GPTDataCL(void);
      GPTDataCL(std::string filename);
      ~GPTDataCL(void);
      void LoadBackupFile(std::string backupFile, int &saveData, int &neverSaveData);
      int DoOptions(int argc, char* argv[]);
}; // class GPTDataCL

int CountColons(char* argument);
uint64_t GetInt(const std::string & argument, int itemNum);
std::string GetString(std::string argument, int itemNum);

#endif
