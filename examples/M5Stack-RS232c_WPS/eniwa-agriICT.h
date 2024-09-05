#ifndef ENIWA_AGRIICT
#define ENIWA_AGRIICT

uint8_t baseCount=4;
char* host[]={
  //Red
  "rtk.toiso.fit",
  //Green
  "rtk.toiso.fit",
  //Blue
//  "117.102.192.33"
  //light blue
  "rtk.toiso.fit",
  "rtk.toiso.fit"
};
int httpPort[]={
  2101,
  2101,
  2101,
  2101
};
char* mntpnt[]={
  "eniwa-bd982",
  "eniwa-bd970",
  "eniwa-bd98223",
  "enwia-bd982-3"
//  "eniwa-f9p"
// "eniwa-kazui"

};
char* user[]={
  "",
  "",
  "",
  "",
};
char* passwd[]={
  "",
  "",
  "",
  ""
};
#endif
