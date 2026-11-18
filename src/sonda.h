#pragma once
#include <string>

// mtsrec --sonda FILE.mov: timecode, fotogrammi e audio in JSON, al posto di ffprobe (sezione 9.81).
int sonda(const std::wstring& percorso);
