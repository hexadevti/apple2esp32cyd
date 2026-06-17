void logSetup() {
    Serial.begin(115200);
}

void printLog(String txt) {
    Serial.println(txt.c_str());
}

void printSequence(int seq) {
  for (int i = 0; i < seq; i++) {
    sprintf(buf,"%02X ", actualBlock[i]);
    printLog(buf);
  }
  Serial.println();
}
  
void printProgress(size_t prg, size_t sz) {
    Serial.printf("Progress: %d%%\n", (prg * 100) / content_len);
  }
  
void printCPUStatus() {
  std::string sFlags = ""; 
  for (int f = 0;f<8;f++) {
    sFlags +=(SR & (1 << f)) != 0 ? "1" : "0";
  }
  sprintf(buf, "[PC]%04X: %02X ,[Addr]%04X(%02X): A=%02X X=%02X Y=%02X FL=%02X(%s) OPFlag=%02X, cycleCount=%d, diffCycleCount=%d", lastPC, opcode, argument_addr, read8(argument_addr), A, X, Y, SR, sFlags.c_str(), opflags, cycleCount, diffCpuCycleCount);
  printLog(buf);
}

void PrintHex(uint8_t data[], int length)
{
  for (int i = 0; i < length; i++)
  {
    if (i % 16 == 0)
    {
      Serial.println();
      sprintf(buf, "%08X: ", i);
      printLog(buf);
    }
    sprintf(buf, "%02X ", data[i]);
    printLog(buf);
  }
  Serial.println();
}
