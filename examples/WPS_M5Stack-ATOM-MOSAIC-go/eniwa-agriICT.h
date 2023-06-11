#ifndef ENIWA_AGRIICT
#define ENIWA_AGRIICT

uint8_t baseCount=2;
char* host[]={
  //Red
  "rtk.toiso.fit",
  //Green
//  "rtk.toiso.fit",
  //Blue
  "rtk.toiso.fit"
  //light blue
//  "rtk.toiso.fit"
};
int httpPort[]={
  2101,
  2101
//  2101,
//  2101
};
char* mntpnt[]={
  "eniwa-bd982",
//  "eniwa-f9p"
//  "eniwa-kazui",
  "eniwa-bd970"
};
char* user[]={
  "",
  ""
//  "",
//  ""
};
char* passwd[]={
  "",
  ""
//  "",
//  ""
};
#endif
