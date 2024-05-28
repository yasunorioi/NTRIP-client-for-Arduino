#ifndef ENIWA_AGRIICT
#define ENIWA_AGRIICT

uint8_t baseCount=3;
char* host[]={
  //Red
  "rtk.toiso.fit",
  //Green
  "rtk.toiso.fit",
  //Blue
  "117.102.192.33"
  //light blue
  //,""
};
int httpPort[]={
  2101,
  2101,
  2101
  //,""
};
char* mntpnt[]={
  "eniwa-bd982",
  "eniwa-bd970",
  "eniwa-kazui"
  //,""
};
char* user[]={
  "",
  "",
  ""
  //,""
};
char* passwd[]={
  "",
  "",
  ""
  //,""
};
#endif
