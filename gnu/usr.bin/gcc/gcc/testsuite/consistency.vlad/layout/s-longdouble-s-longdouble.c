#include <stdio.h>

static struct sss{
  long double f;
  struct {long double m;} snd;
} sss;

#define _offsetof(st,f) ((char *)&((st *) 16)->f - (char *) 16)

int main (void) {
  printf ("+++Struct longdouble inside struct starting with longdouble:\n");
  printf ("size=%d,align=%d\n", sizeof (sss), __alignof__ (sss));
  printf ("offset-longdouble=%d,offset-sss-longdouble=%d,\nalign-longdouble=%d,align-sss-longdouble=%d\n",
          _offsetof (struct sss, f), _offsetof (struct sss, snd),
          __alignof__ (sss.f), __alignof__ (sss.snd));
  return 0;
}
