#ifdef WIN32             // means WIN64, too, oddly
  #include <windows.h>
  #include <sys/time.h>  // gettimeofday for wallclock()
  #include <stdbool.h>   // true and false
#else
  #include <sys/mman.h>  // mmap
  #include <sys/stat.h>  // fstat for filesize
  #include <fcntl.h>     // open
  #include <unistd.h>    // close
  #include <stdbool.h>   // true and false
  #include <ctype.h>     // isspace
  #include <errno.h>     // errno
  #include <string.h>    // strerror
  #include <stdarg.h>    // va_list, va_start
  #include <stdio.h>     // vsnprintf
  #include <math.h>      // ceil, sqrt, isfinite
  #include <time.h>      // clock_gettime for wallclock()
#endif
#include <omp.h>
#include "fread.h"
#include "freadLookups.h"

// Private globals to save passing all of them through to highly iterated field processors
static const char *eof;
static char sep, eol, eol2;
static int eolLen;
static char quote, dec;
static int quoteRule;
static const char **NAstrings;
static int nNAstrings;
static _Bool any_number_like_NAstrings=false;
static _Bool blank_is_a_NAstring=false;
static _Bool stripWhite=true;  // only applies to character columns; numeric fields always stripped
static _Bool skipEmptyLines=false, fill=false;

typedef _Bool (*reader_fun_t)(const char **, void *, int);
static double NA_FLOAT64;  // takes fread.h:NA_FLOAT64_VALUE

#define JUMPLINES 100    // at each of the 100 jumps how many lines to guess column types (10,000 sample lines)

// Private globals so they can be cleaned up both on error and on successful return
const char *fnam=NULL, *mmp=NULL;
size_t fileSize;
_Bool typeOnStack=true;
int8_t *type=NULL;
lenOff *colNames=NULL;
void cleanup() {
  if (!typeOnStack) free(type);
  typeOnStack=true; type=NULL;
  free(colNames); colNames=NULL;
  if (mmp!=NULL) {
    // Important to unmap as OS keeps internal reference open on file. Process is not exiting as
    // we're a .so/.dll here. If this was a process exiting we wouldn't need to unmap.
#ifdef WIN32
    UnmapViewOfFile(mmp);   // TODO - check for error here.
#else
    if (munmap((void *)mmp, fileSize)) DTERROR("%s: '%s'", strerror(errno), fnam);
#endif
  }
  fnam=NULL; mmp=NULL; fileSize=0;
}

void STOP(const char *format, ...) {
    // Solves: http://stackoverflow.com/questions/18597123/fread-data-table-locks-files
    // TODO: always include fnam in the STOP message. For log files etc.
    va_list args;
    va_start(args, format);
    char msg[2000];
    vsnprintf(msg, 2000, format, args);
    va_end(args);
    cleanup();
    DTERROR(msg);
}

static inline size_t umax(size_t a, size_t b) { return a > b ? a : b; }
static inline size_t umin(size_t a, size_t b) { return a < b ? a : b; }

// Helper for error and warning messages to extract next 10 chars or \n if occurs first
// Used exclusively together with "%.*s"
static int STRLIM(const char *ch, size_t limit) {
  size_t maxwidth = umin(limit, (size_t)(eof-ch));
  char *newline = memchr(ch, eol, maxwidth);
  return (newline==NULL ? maxwidth : (size_t)(newline-ch));
}

static void printTypes(signed char *type, int ncol) {
  // e.g. files with 10,000 columns, don't print all of it to verbose output.
  int tt=(ncol<=110?ncol:90); for (int i=0; i<tt; i++) DTPRINT("%d",type[i]);
  if (ncol>110) { DTPRINT("..."); for (int i=ncol-10; i<ncol; i++) DTPRINT("%d",type[i]); }
}

static inline void skip_white(const char **this) {
  // skip space so long as sep isn't space and skip tab so long as sep isn't tab
  const char *ch = *this;
  while(ch<eof && (*ch==' ' || *ch== '\t') && *ch!=sep) ch++;
  *this = ch;
}

static inline _Bool on_sep(const char **this) {
  const char *ch = *this;
  if (sep==' ' && ch<eof && *ch==' ') {
    while (ch+1<eof && *(ch+1)==' ') ch++;  // move to last of this sequence of spaces
    if (ch+1==eof || *(ch+1)==eol) ch++;    // if that's followed by eol or eof then move onto those
  }
  *this = ch;
  return ch>=eof || *ch==sep || *ch==eol;
}

static inline void next_sep(const char **this) {
  const char *ch = *this;
  while (ch<eof && *ch!=sep && *ch!=eol) ch++;
  on_sep(&ch); // to deal with multiple spaces when sep==' '
  *this = ch;
}

static inline _Bool is_NAstring(const char *fieldStart) {
  skip_white(&fieldStart);  // updates local fieldStart
  for (int i=0; i<nNAstrings; i++) {
    const char *ch1 = fieldStart;
    const char *ch2 = NAstrings[i];
    while (ch1<eof && *ch1==*ch2) { ch1++; ch2++; }  // not using strncmp due to eof not being '\0'
    if (*ch2=='\0') {
      skip_white(&ch1);
      if (ch1>=eof || *ch1==sep || *ch1==eol) return true;
      // if "" is in NAstrings then true will be returned as intended
    }
  }
  return false;
}

static _Bool Field(const char **this, void *targetCol, int targetRow)
{
  const char *ch = *this;
  if (stripWhite) skip_white(&ch);  // before and after quoted field's quotes too (e.g. test 1609) but never inside quoted fields
  const char *fieldStart=ch;
  _Bool quoted = false;
  if (*ch!=quote || quoteRule==3) {
    // unambiguously not quoted. simply search for sep|eol. If field contains sep|eol then it must be quoted instead.
    while(ch<eof && *ch!=sep && *ch!=eol) ch++;
  } else {
    // the field is quoted and quotes are correctly escaped (quoteRule 0 and 1)
    // or the field is quoted but quotes are not escaped (quoteRule 2)
    // or the field is not quoted but the data contains a quote at the start (quoteRule 2 too)
    int eolCount = 0;
    quoted = true;
    fieldStart = ch+1; // step over opening quote
    switch(quoteRule) {
    case 0:  // quoted with embedded quotes doubled; the final unescaped " must be followed by sep|eol 
      while (++ch<eof && eolCount<100) {  // TODO: expose this 100 to user to allow them to increase
        eolCount += (*ch==eol);
        // 100 prevents runaway opening fields by limiting eols. Otherwise the whole file would be read in the sep and
        // quote rule testing step.
        if (*ch==quote) {
          if (ch+1<eof && *(ch+1)==quote) { ch++; continue; }
          break;  // found undoubled closing quote
        }
      }
      if (ch>=eof || *ch!=quote) return false;
      break;
    case 1:  // quoted with embedded quotes escaped; the final unescaped " must be followed by sep|eol
      while (++ch<eof && *ch!=quote && eolCount<100) {
        eolCount += (*ch==eol);
        ch += (*ch=='\\');
      }
      if (ch>=eof || *ch!=quote) return false;
      break;
    case 2:  // (i) quoted (perhaps because the source system knows sep is present) but any quotes were not escaped at all,
             // so look for ", to define the end.   (There might not be any quotes present to worry about, anyway).
             // (ii) not-quoted but there is a quote at the beginning so it should have been, look for , at the end
             // If no eol are present in the data (i.e. rows are rows), then this should work ok e.g. test 1453
             // since we look for ", and the source system quoted when , is present, looking for ", should work well.
             // No eol may occur inside fields, under this rule.
      {
        const char *ch2 = ch;  
        while (++ch<eof && *ch!=eol) {
          if (*ch==quote && (ch+1>=eof || *(ch+1)==sep || *(ch+1)==eol)) {ch2=ch; break;}   // (*1) regular ", ending
          if (*ch==sep) {
            // first sep in this field
            // if there is a ", afterwards but before the next \n, use that; the field was quoted and it's still case (i) above.
            // Otherwise break here at this first sep as it's case (ii) above (the data contains a quote at the start and no sep)
            ch2 = ch;
            while (++ch2<eof && *ch2!=eol) {
              if (*ch2==quote && (ch2+1>=eof || *(ch2+1)==sep || *(ch2+1)==eol)) {
                ch = ch2; // (*2) move on to that first ", -- that's this field's ending
                break;
              }
            }
            break;
          }
        }
        if (ch!=ch2) { fieldStart--; quoted=false; } // field ending is this sep (neither (*1) or (*2) happened)
      }
      break;
    default:
      return false;  // Internal error: undefined quote rule
    }
  }
  int fieldLen = (int)(ch-fieldStart);
  if (stripWhite && !quoted) {
    while(fieldLen>0 && (fieldStart[fieldLen-1]==' ' || fieldStart[fieldLen-1]=='\t')) fieldLen--;
    // this white space (' ' or '\t') can't be sep otherwise it would have stopped the field earlier at the first sep
  }
  if (quoted) { ch++; if (stripWhite) skip_white(&ch); }
  if (!on_sep(&ch)) return false;
  if (targetCol) {
    if (fieldLen==0) {
      if (blank_is_a_NAstring) fieldLen=INT_MIN;
    } else {
      if (is_NAstring(fieldStart)) fieldLen=INT_MIN;
    }
    ((lenOff *)targetCol)[targetRow].len = fieldLen;
    ((lenOff *)targetCol)[targetRow].off = (uint32_t)(fieldStart-*this);  // agnostic & thread-safe
  }
  *this = ch; // Update caller's ch. This may be after fieldStart+fieldLen due to quotes and/or whitespace
  return true;
}

static _Bool SkipField(const char **this, void *targetCol, int targetRow)
{
   // wrapper around Field for CT_DROP to save a branch in the main data reader loop and
   // to make the *fun[] lookup a bit clearer
   return Field(this,NULL,0);
}

static inline int countfields(const char **this)
{
  const char *ch = *this;
  if (sep==' ') while (ch<eof && *ch==' ') ch++;  // Correct to be sep==' ' only and not skip_white(). 
  int ncol = 1;
  while (ch<eof && *ch!=eol) {
    if (!Field(&ch,NULL,0)) return -1;   // -1 means this line not valid for this sep and quote rule
    // Field() leaves *ch resting on sep, eol or >=eof.  (Checked inside Field())
    if (ch<eof && *ch!=eol) { ncol++; ch++; } // move over sep (which will already be last ' ' if sep=' '). TODO. Can be removed this line?
                //   ^^  Not *ch==sep because sep==eol when readLines
  }
  ch += eolLen; // may step past eof but that's ok as we never use ==eof in this file, always >=eof or <eof.
  *this = ch;
  return ncol;
}

static inline _Bool nextGoodLine(const char **this, int ncol)
{
  const char *ch = *this;
  // we may have landed inside quoted field containing embedded sep and/or embedded \n
  // find next \n and see if 5 good lines follow. If not try next \n, and so on, until we find the real \n
  // We don't know which line number this is, either, because we jumped straight to it. So return true/false for
  // the line number and error message to be worked out up there.
  int attempts=0;
  while (ch<eof && attempts++<30) {
    while (ch<eof && *ch!=eol) ch++;
    if (ch<eof) ch+=eolLen;
    int i = 0, thisNcol=0;
    const char *ch2 = ch;
    while (ch2<eof && i<5 && ( (thisNcol=countfields(&ch2))==ncol || (thisNcol==0 && (skipEmptyLines || fill)))) i++;
    if (i==5 || ch2>=eof) break;
  }
  if (ch<eof && attempts<30) { *this = ch; return true; }
  return false;
}

static _Bool StrtoI64(const char **this, void *targetCol, int targetRow)
{
    // Specialized clib strtoll that :
    // i) skips leading isspace() too but other than field separator and eol (e.g. '\t' and ' \t' in FUT1206.txt)
    // ii) has fewer branches for speed as no need for non decimal base
    // iii) updates global ch directly saving arguments
    // iv) safe for mmap which can't be \0 terminated on Windows (but can be on unix and mac)
    // v) fails if whole field isn't consumed such as "3.14" (strtol consumes the 3 and stops)
    // ... all without needing to read into a buffer at all (reads the mmap directly)
    const char *ch = *this;
    skip_white(&ch);  //  ',,' or ',   ,' or '\t\t' or '\t   \t' etc => NA
    if (on_sep(&ch)) {  // most often ',,' 
      if (targetCol) ((int64_t *)targetCol)[targetRow] = NA_INT64;
      *this = ch;
      return true;
    }
    const char *start=ch;
    int sign=1;
    _Bool quoted = false;
    if (ch<eof && (*ch==quote)) { quoted=true; ch++; }
    if (ch<eof && (*ch=='-' || *ch=='+')) sign -= 2*(*ch++=='-');
    _Bool ok = ch<eof && '0'<=*ch && *ch<='9';  // a single - or + with no [0-9] is !ok and considered type character
    int64_t acc = 0;
    while (ch<eof && '0'<=*ch && *ch<='9' && acc<(INT64_MAX-10)/10) { // compiler should optimize last constant expression
      // Conveniently, INT64_MIN == -9223372036854775808 and INT64_MAX == +9223372036854775807
      // so the full valid range is now symetric [-INT64_MAX,+INT64_MAX] and NA==INT64_MIN==-INT64_MAX-1
      acc *= 10;
      acc += *ch-'0';
      ch++;
    }
    if (quoted) { if (ch>=eof || *ch!=quote) return false; else ch++; }
    // TODO: if (!targetCol) return early?  Most of the time, not though. 
    if (targetCol) ((int64_t *)targetCol)[targetRow] = sign * acc;
    skip_white(&ch);
    ok = ok && on_sep(&ch);
    //DTPRINT("StrtoI64 field '%.*s' has len %d\n", lch-ch+1, ch, len);
    *this = ch;
    if (ok && !any_number_like_NAstrings) return true;  // most common case, return 
    _Bool na = is_NAstring(start);
    if (ok && !na) return true;
    if (targetCol) ((int64_t *)targetCol)[targetRow] = NA_INT64;
    next_sep(&ch);  // TODO: can we delete this? consume the remainder of field, if any
    *this = ch;
    return na;
}    

static _Bool StrtoI32(const char **this, void *targetCol, int targetRow)
{
    // Very similar to StrtoI64 (see it for comments). We can't make a single function and switch on TYPEOF(targetCol) to
    // know I64 or I32 because targetCol is NULL when testing types and when dropping columns.
    const char *ch = *this;
    skip_white(&ch);
    if (on_sep(&ch)) {  // most often ',,' 
      if (targetCol) ((int32_t *)targetCol)[targetRow] = NA_INT32;
      *this = ch;
      return true;
    }
    const char *start=ch;
    int sign=1;
    _Bool quoted = false;
    if (ch<eof && (*ch==quote)) { quoted=true; ch++; }
    if (ch<eof && (*ch=='-' || *ch=='+')) sign -= 2*(*ch++=='-');
    _Bool ok = ch<eof && '0'<=*ch && *ch<='9';
    int acc = 0;
    while (ch<eof && '0'<=*ch && *ch<='9' && acc<(INT32_MAX-10)/10) {  // NA==INT_MIN==-2147483648==-INT_MAX(+2147483647)-1
      acc *= 10;
      acc += *ch-'0';
      ch++;
    }
    if (quoted) { if (ch>=eof || *ch!=quote) return false; else ch++; }
    if (targetCol) ((int32_t *)targetCol)[targetRow] = sign * acc;
    skip_white(&ch);
    ok = ok && on_sep(&ch);
    //DTPRINT("StrtoI32 field '%.*s' has len %d\n", lch-ch+1, ch, len);
    *this = ch;
    if (ok && !any_number_like_NAstrings) return true;
    _Bool na = is_NAstring(start);
    if (ok && !na) return true;
    if (targetCol) ((int32_t *)targetCol)[targetRow] = NA_INT32;
    next_sep(&ch);
    *this = ch;
    return na;
}


// generate freadLookups.h
// TODO: review ERANGE checks and tests; that range outside [1.7e-308,1.7e+308] coerces to [0.0,Inf]
/*
f = "~/data.table/src/freadLookups.h"
cat("const long double pow10lookup[701] = {\n", file=f, append=FALSE)
for (i in (-350):(349)) cat("1.0E",i,"L,\n", sep="", file=f, append=TRUE)
cat("1.0E350L\n};\n", file=f, append=TRUE)
*/

static _Bool StrtoD(const char **this, void *targetCol, int targetRow)
{
    // [+|-]N.M[E|e][+|-]E or Inf or NAN
    
    const char *ch = *this;
    skip_white(&ch);
    if (on_sep(&ch)) {
      if (targetCol) ((double *)targetCol)[targetRow] = NA_FLOAT64;
      *this = ch;
      return true;
    }
    _Bool quoted = false;
    if (ch<eof && (*ch==quote)) { quoted=true; ch++; }
    int sign=1;
    double d=NAN;
    if (ch<eof && (*ch=='-' || *ch=='+')) sign -= 2*(*ch++=='-');
    const char *start=ch;
    _Bool ok = ch<eof && (('0'<=*ch && *ch<='9') || *ch==dec);  // a single - or + with no [0-9] is !ok and considered type character
    if (!ok) {
      if      (ch<eof && *ch=='I' && *(ch+1)=='n' && *(ch+2)=='f') { ch+=3; d=sign*INFINITY; ok=true; }
      else if (ch<eof && *ch=='N' && *(ch+1)=='A' && *(ch+2)=='N') { ch+=3; d=NAN; ok=true; }
    } else {
      uint64_t acc = 0;
      while (ch<eof && '0'<=*ch && *ch<='9' && acc<(UINT64_MAX-10)/10) { // compiler should optimize last constant expression
        // UNIT64_MAX == 18446744073709551615
        acc *= 10;
        acc += *ch-'0';
        ch++;
      }
      const char *decCh = (ch<eof && *ch==dec) ? ++ch : NULL;
      while (ch<eof && '0'<=*ch && *ch<='9' && acc<(UINT64_MAX-10)/10) {
        acc *= 10;
        acc += *ch-'0';
        ch++;
      }
      int e = decCh ? -(int)(ch-decCh) : 0;
      if (decCh) while (ch<eof && '0'<=*ch && *ch<='9') ch++; // lose precision
      else       while (ch<eof && '0'<=*ch && *ch<='9') { e--; ch++; }  // lose precision but retain scale
      if (ch<eof && (*ch=='E' || *ch=='e')) {
        ch++;
        int esign=1;
        if (ch<eof && (*ch=='-' || *ch=='+')) esign -= 2*(*ch++=='-');
        int eacc = 0;
        while (ch<eof && '0'<=*ch && *ch<='9' && eacc<(INT32_MAX-10)/10) {
          eacc *= 10;
          eacc += *ch-'0';
          ch++;
        }
        e += esign * eacc;
      }
      d = (double)(sign * (long double)acc * pow10lookup[350+e]);
    }
    if (quoted) { if (ch>=eof || *ch!=quote) return false; else ch++; }
    if (targetCol) ((double *)targetCol)[targetRow] = d;
    skip_white(&ch);
    ok = ok && on_sep(&ch); 
    *this = ch;
    if (ok && !any_number_like_NAstrings) return true;
    _Bool na = is_NAstring(start);
    if (ok && !na) return true;
    if (targetCol) ((double *)targetCol)[targetRow] = NA_FLOAT64;
    next_sep(&ch);
    *this = ch;
    return na;
}

static _Bool StrtoB(const char **this, void *targetCol, int targetRow)
{
    // These usually come from R when it writes out.
    const char *ch = *this;
    skip_white(&ch);
    if (targetCol) ((int8_t *)targetCol)[targetRow] = NA_BOOL8;
    if (on_sep(&ch)) { *this=ch; return true; }  // empty field ',,'
    const char *start=ch;
    _Bool quoted = false;
    if (ch<eof && (*ch==quote)) { quoted=true; ch++; }
    if (quoted && *ch==quote) { ch++; if (on_sep(&ch)) {*this=ch; return true;} else return false; }  // empty quoted field ',"",'
    _Bool logical01 = false;  // expose to user and should default be true?
    if ( ((*ch=='0' || *ch=='1') && logical01) || (*ch=='N' && ch+1<eof && *(ch+1)=='A' && ch++)) {
        if (targetCol) ((int8_t *)targetCol)[targetRow] = (*ch=='1' ? true : (*ch=='0' ? false : NA_BOOL8));
        ch++;
    } else if (*ch=='T') {
        if (targetCol) ((int8_t *)targetCol)[targetRow] = true;
        if (++ch+2<eof && ((*ch=='R' && *(ch+1)=='U' && *(ch+2)=='E') ||
                           (*ch=='r' && *(ch+1)=='u' && *(ch+2)=='e'))) ch+=3;
    } else if (*ch=='F') {
        if (targetCol) ((int8_t *)targetCol)[targetRow] = false;
        if (++ch+3<eof && ((*ch=='A' && *(ch+1)=='L' && *(ch+2)=='S' && *(ch+3)=='E') ||
                           (*ch=='a' && *(ch+1)=='l' && *(ch+2)=='s' && *(ch+3)=='e'))) ch+=4;
    }
    if (quoted) { if (ch>=eof || *ch!=quote) return false; else ch++; }
    if (on_sep(&ch)) { *this=ch; return true; }
    if (targetCol) ((int8_t *)targetCol)[targetRow] = NA_BOOL8;
    next_sep(&ch);
    *this=ch;
    return is_NAstring(start);
}

static reader_fun_t fun[NUMTYPE] = {&SkipField, &StrtoB, &StrtoI32, &StrtoI64, &StrtoD, &Field};

static double wallclock()
{
    double ans = 0;
#ifdef CLOCK_REALTIME
    struct timespec tp;
    if (0==clock_gettime(CLOCK_REALTIME, &tp))
        ans = (double) tp.tv_sec + 1e-9 * (double) tp.tv_nsec;
#else
    struct timeval tv;
    if (0==gettimeofday(&tv, NULL))
        ans = (double) tv.tv_sec + 1e-6 * (double) tv.tv_usec;
#endif
    return ans;
}

// TODO return int and strerror,  or macro-ise ERROR
void freadMain(freadMainArgs args) {
    double t0 = wallclock();
    
    if (args.nth==0) STOP("nThread==0");
    typeOnStack = false;
    type = NULL;
    uint64_t ui64 = NA_FLOAT64_I64;
    memcpy(&NA_FLOAT64, &ui64, 8);
    
    NAstrings = args.NAstrings;
    nNAstrings = args.nNAstrings;
    any_number_like_NAstrings = false;
    blank_is_a_NAstring = false;
    // if we know there are no nastrings which are numbers (like -999999) then in the number
    // field processors we can save an expensive step in checking the NAstrings. If the field parses as a number,
    // we then when any_number_like_nastrings==FALSE we know it can't be NA.
    for (int i=0; i<nNAstrings; i++) {
      if (NAstrings[i][0]=='\0') {blank_is_a_NAstring=true; continue; }
      const char *ch=NAstrings[i];
      int nchar = strlen(ch);
      if (isspace(ch[0]) || isspace(ch[nchar-1]))
        STOP("fread_main: NAstrings[%d]==<<%s>> has whitespace at the beginning or end", i+1, ch);
      if (strcmp(ch,"T")==0    || strcmp(ch,"F")==0 ||
          strcmp(ch,"TRUE")==0 || strcmp(ch,"FALSE")==0 ||
          strcmp(ch,"True")==0 || strcmp(ch,"False")==0 ||
          strcmp(ch,"1")==0    || strcmp(ch,"0")==0)
        STOP("fread_main: NAstrings[%d]==<<%s>> is recognized as type boolean. This is not permitted.", i+1, ch);
      char *end;
      errno = 0;
      strtod(ch, &end);  // careful not to let "" get to here (see continue above) as strtod considers "" numeric
      if (errno==0 && (int)(end-ch)==nchar) any_number_like_NAstrings = true;
    }
    if (args.verbose) {
      DTPRINT("Parameter NAstrings == ");
      if (nNAstrings==0) DTPRINT("None\n");
      else { for (int i=0; i<nNAstrings; i++) { DTPRINT(i==0 ? "<<%s>>" : ", <<%s>>", NAstrings[i]); }; DTPRINT("\n"); }
      DTPRINT("%s of the %d na.strings are numeric (such as '-9999').\n",
            any_number_like_NAstrings ? "One or more" : "None", nNAstrings);
    }
    
    stripWhite = args.stripWhite;
    skipEmptyLines = args.skipEmptyLines;
    fill = args.fill;
    dec = args.dec;
    quote = args.quote;
    
    // ********************************************************************************************
    //   Point to text input if it contains \n, or open and mmap file if not
    // ********************************************************************************************
    const char *ch, *sof;
    fnam = NULL;
    mmp = NULL;
    ch = args.input;
    while (*ch!='\0' && *ch!='\n') ch++;
    if (*ch=='\n' || args.input[0]=='\0') {
        if (args.verbose) DTPRINT("Input contains a \\n (or is \"\"). Taking this to be text input (not a filename)\n");
        fileSize = strlen(args.input);
        sof = args.input;
        eof = sof+fileSize;
        if (*eof!='\0') STOP("Internal error: last byte of character input isn't \\0");
    } else {
        if (args.verbose) DTPRINT("Input contains no \\n. Taking this to be a filename to open\n");
        fnam = args.input;
#ifndef WIN32
        int fd = open(fnam, O_RDONLY);
        if (fd==-1) STOP("file not found: %s",fnam);
        struct stat stat_buf;
        if (fstat(fd,&stat_buf) == -1) {close(fd); STOP("Opened file ok but couldn't obtain file size: %s", fnam);}
        fileSize = stat_buf.st_size;
        if (fileSize<=0) {close(fd); STOP("File is empty: %s", fnam);}
        if (args.verbose) DTPRINT("File opened, size %.6f GB.\nMemory mapping ... ", 1.0*fileSize/(1024*1024*1024));
        
        // No MAP_POPULATE for faster nrows=10 and to make possible earlier progress bar in row count stage
        // Mac doesn't appear to support MAP_POPULATE anyway (failed on CRAN when I tried).
        // TO DO?: MAP_HUGETLB for Linux but seems to need admin to setup first. My Hugepagesize is 2MB (>>2KB, so promising)
        //         https://www.kernel.org/doc/Documentation/vm/hugetlbpage.txt
        mmp = (const char *)mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);  // we don't need to keep file handle open
        if (mmp == MAP_FAILED) {
#else
        // Following: http://msdn.microsoft.com/en-gb/library/windows/desktop/aa366548(v=vs.85).aspx
        HANDLE hFile = INVALID_HANDLE_VALUE;
        int attempts = 0;
        while(hFile==INVALID_HANDLE_VALUE && attempts<5) {
            hFile = CreateFile(fnam, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            // FILE_SHARE_WRITE is required otherwise if the file is open in Excel, CreateFile fails. Should be ok now.
            if (hFile==INVALID_HANDLE_VALUE) {
                if (GetLastError()==ERROR_FILE_NOT_FOUND) STOP("File not found: %s",fnam);
                if (attempts<4) Sleep(250);  // 250ms
            }
            attempts++;
            // Looped retry to avoid ephemeral locks by system utilities as recommended here : http://support.microsoft.com/kb/316609
        }
        if (hFile==INVALID_HANDLE_VALUE) STOP("Unable to open file after %d attempts (error %d): %s", attempts, GetLastError(), fnam);
        LARGE_INTEGER liFileSize;
        if (GetFileSizeEx(hFile,&liFileSize)==0) { CloseHandle(hFile); STOP("GetFileSizeEx failed (returned 0) on file: %s", fnam); }
        fileSize = (size_t)liFileSize.QuadPart;
        if (fileSize<=0) { CloseHandle(hFile); STOP("File is empty: %s", fnam); }
        HANDLE hMap=CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL); // fileSize+1 not allowed here, unlike mmap where +1 is zero'd
        if (hMap==NULL) { CloseHandle(hFile); STOP("This is Windows, CreateFileMapping returned error %d for file %s", GetLastError(), fnam); }
        if (args.verbose) DTPRINT("File opened, size %.6f GB.\nMemory mapping ... ", 1.0*fileSize/(1024*1024*1024));
        mmp = (const char *)MapViewOfFile(hMap,FILE_MAP_READ,0,0,fileSize);
        CloseHandle(hMap);  // we don't need to keep the file open; the MapView keeps an internal reference;
        CloseHandle(hFile); //   see https://msdn.microsoft.com/en-us/library/windows/desktop/aa366537(v=vs.85).aspx
        if (mmp == NULL) {
#endif
            if (sizeof(char *)==4)
                STOP("Opened file ok, obtained its size on disk (%.1fMB) but couldn't memory map it. This is a 32bit machine. You don't need more RAM per se but this fread function is tuned for 64bit addressability at the expense of large file support on 32bit machines. You probably need more RAM to store the resulting data.table, anyway. And most speed benefits of data.table are on 64bit with large RAM, too. Please upgrade to 64bit.", fileSize/(1024.0*1024));
                // if we support this on 32bit, we may need to use stat64 instead, as R does
            else if (sizeof(char *)==8)
                STOP("Opened file ok, obtained its size on disk (%.1fMB), but couldn't memory map it. This is a 64bit machine so this is surprising. Please report to datatable-help.", fileSize/1024^2);
            else
                STOP("Opened file ok, obtained its size on disk (%.1fMB), but couldn't memory map it. Size of pointer is %d on this machine. Probably failing because this is neither a 32bit or 64bit machine. Please report to datatable-help.", fileSize/1024^2, sizeof(char *));
        }
        sof = mmp;
        eof = sof+fileSize;  // byte after last byte of file.  Never dereference eof as it's not mapped.
        if (args.verbose) DTPRINT("ok\n");  // to end 'Memory mapping ... '
    }
    double tMap = wallclock();
    
    // ********************************************************************************************
    //   Auto detect eol, first eol where there are two (i.e. CRLF)
    // ********************************************************************************************
    // take care of UTF8 BOM, #1087 and #1465
    if (!memcmp(sof, "\xef\xbb\xbf", 3)) sof += 3;
    ch = sof;
    while (ch<eof && *ch!='\n' && *ch!='\r') {
        if (*ch==quote) while(++ch<eof && *ch!=quote) {}; // (TODO unbounded to fix) allows protection of \n and \r inside column names
        ch++;                                             // this 'if' needed in case opening protection is not closed before eof
    }
    if (ch>=eof) {
        if (ch>eof) STOP("Internal error: ch>eof when detecting eol");
        if (args.verbose) DTPRINT("Input ends before any \\r or \\n observed. Input will be treated as a single row.\n");
        eol=eol2='\n'; eolLen=1;
    } else {
        eol=eol2=*ch; eolLen=1;
        if (eol=='\r') {
            if (ch+1<eof && *(ch+1)=='\n') {
                if (args.verbose) DTPRINT("Detected eol as \\r\\n (CRLF) in that order, the Windows standard.\n");
                eol2='\n'; eolLen=2;
            } else {
                if (ch+1<eof && *(ch+1)=='\r')
                    STOP("Line ending is \\r\\r\\n. R's download.file() appears to add the extra \\r in text mode on Windows. Please download again in binary mode (mode='wb') which might be faster too. Alternatively, pass the URL directly to fread and it will download the file in binary mode for you.");
                    // NB: on Windows, download.file from file: seems to condense \r\r too. So 
                if (args.verbose) DTPRINT("Detected eol as \\r only (no \\n or \\r afterwards). An old Mac 9 standard, discontinued in 2002 according to Wikipedia.\n");
            }
        } else if (eol=='\n') {
            if (ch+1<eof && *(ch+1)=='\r') {
                DTWARN("Detected eol as \\n\\r, a highly unusual line ending. According to Wikipedia the Acorn BBC used this. If it is intended that the first column on the next row is a character column where the first character of the field value is \\r (why?) then the first column should start with a quote (i.e. 'protected'). Proceeding with attempt to read the file.\n");
                eol2='\r'; eolLen=2;
            } else if (args.verbose) DTPRINT("Detected eol as \\n only (no \\r afterwards), the UNIX and Mac standard.\n");
        } else
            STOP("Internal error: if no \\r or \\n found then ch should be eof");
    }

    // ********************************************************************************************
    //   Position to line skip+1 or line containing skip="string"
    // ********************************************************************************************
    int line = 1;
    // line is for error and warning messages so considers raw \n whether inside quoted fields or not, just
    // like wc -l, head -n and tail -n
    const char *pos = sof;
    ch = pos;
    if (args.skipString!=NULL) {
        ch = strstr(sof, args.skipString);
        if (!ch) STOP("skip='%s' not found in input (it is case sensitive and literal; i.e., no patterns, wildcards or regex)",
                      args.skipString);
        while (ch>sof && *(ch-1)!=eol2) ch--;  // move to beginning of line
        pos = ch;
        ch = sof;
        while (ch<pos) line+=(*ch++==eol);
        if (args.verbose) DTPRINT("Found skip='%s' on line %d. Taking this to be header row or first row of data.\n",
                                  args.skipString, line);
        ch = pos;
    } else if (args.skipNrow>0) {
        while (ch<eof && line<=args.skipNrow) line+=(*ch++==eol);
        if (ch>=eof) STOP("skip=%d but the input only has %d line%s", args.skipNrow, line, line>1?"s":"");
        ch += (eolLen-1); // move over eol2 on Windows to be on start of desired line
        pos = ch;
    }
    
    // skip blank input at the start
    const char *lineStart = ch;
    while (ch<eof && isspace(*ch)) {   // isspace matches ' ', \t, \n and \r
      if (*ch==eol) { ch+=eolLen; lineStart=ch; line++; } else ch++;
    }
    if (ch>=eof) STOP("Input is either empty, fully whitespace, or skip has been set after the last non-whitespace.");
    if (args.verbose) {
      if (lineStart>ch) DTPRINT("Moved forward to first non-blank line (%d)\n", line);
      DTPRINT("Positioned on line %d starting: <<%.*s>>\n", line, STRLIM(lineStart, 30), lineStart);
    }
    ch = pos = lineStart;
    
    // *********************************************************************************************************
    //   Auto detect separator, quoting rule, first line and ncol, simply, using jump 0 only
    // *********************************************************************************************************
    char seps[]=",|;\t ";  // default seps in order of preference. See ?fread.    
    // using seps[] not *seps for writeability (http://stackoverflow.com/a/164258/403310)
    int nseps=strlen(seps);
    
    if (args.sep == quote && quote!='\0') STOP("sep == quote ('%c') is not allowed", quote);
    if (dec=='\0') STOP("dec='' not allowed. Should be '.' or ','");
    if (args.sep == dec) STOP("sep == dec ('%c') is not allowed", dec);
    if (quote == dec) STOP("quote == dec ('%c') is not allowed", dec);
    if (args.sep == '\0') {  // default is '\0' meaning 'auto'
      if (args.verbose) DTPRINT("Detecting sep ...\n");
    } else {
      seps[0]=args.sep; seps[1]='\0'; nseps=1;
      if (args.verbose) DTPRINT("Using supplied sep '%s'\n", args.sep=='\t' ? "\\t" : seps);
    }
    
    int topNumLines=0;        // the most number of lines with the same number of fields, so far
    int topNumFields=1;       // how many fields that was, to resolve ties
    int topNmax=0;            // for that sep and quote rule, what was the max number of columns (just for fill=true)
    char topSep=eol;          // which sep that was, by default \n to mean single-column input (1 field)
    int topQuoteRule=0;       // which quote rule that was
    const char *firstJumpEnd=eof; // remember where the winning jumpline from jump 0 ends, to know its size excluding header

    // Always sample as if nrows= wasn't supplied. That's probably *why* user is setting nrow=0 to get the column names
    // and types, without actually reading the data yet. Most likely to check consistency across a set of files.
    int numFields[JUMPLINES+1];   // +1 to cover header row. Don't know at this stage whether it is present or not.
    int numLines[JUMPLINES+1];
    
    for (int s=0; s<nseps; s++) {
      sep = seps[s];
      for (quoteRule=0; quoteRule<4; quoteRule++) {  // quote rule in order of preference
        ch = pos;
        // DTPRINT("Trying sep='%c' with quoteRule %d ...", sep, quoteRule);
        for (int i=0; i<=JUMPLINES; i++) { numFields[i]=0; numLines[i]=0; } // clear VLAs
        int i=-1; // The slot we're counting the currently contiguous consistent ncol
        int thisLine=0, lastncol=0;
        while (ch<eof && thisLine++<=JUMPLINES) {
          int thisncol = countfields(&ch);   // using this sep and quote rule; moves ch to start of next line
          if (thisncol<0) { numFields[0]=-1; break; }  // invalid file with this sep and quote rule; abort this rule
          if (lastncol!=thisncol) { numFields[++i]=thisncol; lastncol=thisncol; } // new contiguous consistent ncol started
          numLines[i]++;
        }
        if (numFields[0]==-1) continue;
        _Bool updated=false;
        int nmax=0;
        
        i=-1; while (numLines[++i]) {
          if (numFields[i] > nmax) nmax=numFields[i];  // for fill=true to know max number of columns
          if (numFields[i]>1 &&    // the default sep='\n' (whole lines, single column) shuld take precedence 
              ( numLines[i]>topNumLines ||
               (numLines[i]==topNumLines && numFields[i]>topNumFields && sep!=' '))) {
            topNumLines=numLines[i]; topNumFields=numFields[i]; topSep=sep; topQuoteRule=quoteRule; topNmax=nmax;
            firstJumpEnd = ch;  // So that after the header we know how many bytes jump point 0 is
            updated = true;
            // Two updates can happen for the same sep and quoteRule (e.g. issue_1113_fread.txt where sep=' ') so the
            // updated flag is just to print once.
          }
        }
        if (args.verbose && updated) {
          DTPRINT("  sep=="); DTPRINT(sep=='\t' ? "'\\t'" : "'%c'(ascii %d)", sep, sep);
          DTPRINT("  with %d lines of %d fields using quote rule %d\n", topNumLines, topNumFields, topQuoteRule);
        }
      }
    }
    
    int ncol;
    quoteRule = topQuoteRule;
    sep = topSep;
    ch = pos;
    if (fill) {
      // start input from first populated line, already pos.
      ncol = topNmax;
    } else {
      // find the top line with the consistent number of fields.  There might be irregular header lines above it.
      //int nmax=0, thisLine=-1, whichmax=0;
      ncol = topNumFields;
      int thisLine=-1; while (ch<eof && ++thisLine<JUMPLINES) {
        const char *lineStart = ch;
        if (countfields(&ch)==ncol) { ch=pos=lineStart; line+=thisLine; break; }
      }
    }
    // For standard regular separated files, we're now on the first byte of the file.
    
    if (ncol<1 || line<1) STOP("Internal error: ncol==%d line==%d after detecting sep, ncol and first line");
    int tt = countfields(&ch);
    ch = pos; // move back to start of line since countfields() moved to next
    if (!fill && tt!=ncol) STOP("Internal error: first line has field count %d but expecting %d", tt, ncol);
    if (args.verbose) {
      DTPRINT("Detected %d columns on line %d. This line is either column names or first data row (first 30 chars): <<%.*s>>\n",
               tt, line, STRLIM(pos, 30), pos);
      if (fill) DTPRINT("fill=true and the most number of columns found is %d\n", ncol);
    }
    
    // ********************************************************************************************
    //   Detect and assign column names (if present)
    // ********************************************************************************************
    const char *colNamesAnchor = ch;
    colNames = calloc(ncol, sizeof(lenOff));
    if (!colNames) STOP("Unable to allocate %d*%d bytes for column name pointers: %s", ncol, sizeof(lenOff), strerror(errno));
    _Bool allchar=true;
    if (sep==' ') while (ch<eof && *ch==' ') ch++;
    ch--;  // so we can ++ at the beginning inside loop.
    for (int field=0; field<tt; field++) {
      const char *this = ++ch;
      //DTPRINT("Field %d <<%.*s>>\n", i, STRLIM(ch,20), ch);
      skip_white(&ch);
      if (allchar && !on_sep(&ch) && StrtoD(&ch,NULL,0)) allchar=false;  // don't stop early as we want to check all columns to eol here
      // considered looking for one isalpha present but we want 1E9 to be considered a value not a column name
      ch = this;  // rewind to the start of this field
      Field(&ch,NULL,0);  // StrtoD does not consume quoted fields according to the quote rule, so redo with Field()
      // countfields() above already validated the line so no need to check again now.
    }
    if (ch<eof && *ch!=eol)
      STOP("Read %d expected fields in the header row (fill=%d) but finished on <<%.*s>>'",tt,fill,STRLIM(ch,30),ch);
    // already checked above that tt==ncol unless fill=TRUE
    // when fill=TRUE and column names shorter (test 1635.2), leave calloc initialized lenOff.len==0
    if (args.verbose && args.header!=NA_BOOL8) DTPRINT("'header' changed by user from 'auto' to %s\n", args.header?"true":"false");
    if (args.header==false || (args.header==NA_BOOL8 && !allchar)) {
        if (args.verbose && args.header==NA_BOOL8) DTPRINT("Some fields on line %d are not type character. Treating as a data row and using default column names.\n", line);
        // colNames was calloc'd so nothing to do; all len=off=0 already
        ch = pos;  // back to start of first row. Treat as first data row, no column names present.
        // now check previous line which is being discarded and give helpful msg to user ...
        if (ch>sof && args.skipNrow==0) {
          ch -= (eolLen+1);
          if (ch<sof) ch=sof;  // for when sof[0]=='\n'
          while (ch>sof && *ch!=eol2) ch--;
          if (ch>sof) ch++;
          const char *prevStart = ch;
          int tmp = countfields(&ch);
          if (tmp==ncol) STOP("Internal error: row before first data row has the same number of fields but we're not using it.");
          if (tmp>1) DTWARN("Starting data input on line %d <<%.*s>> with %d fields and discarding line %d <<%.*s>> before it because it has a different number of fields (%d).", line, STRLIM(pos, 30), pos, ncol, line-1, STRLIM(prevStart, 30), prevStart, tmp);
        }
        if (ch!=pos) STOP("Internal error. ch!=pos after prevBlank check");     
    } else {
        if (args.verbose && args.header==NA_BOOL8) DTPRINT("All the fields on line %d are character fields. Treating as the column names.\n", line);
        ch = pos;
        line++;
        if (sep==' ') while (ch<eof && *ch==' ') ch++;
        ch--;
        for (int i=0; i<ncol; i++) {
            // Use Field() here as it's already designed to handle quotes, leading space etc.
            const char *start = ++ch;
            Field(&ch, colNames, i);  // stores the string length and offset as <uint,uint> in colnames[i]
            colNames[i].off += (size_t)(start-colNamesAnchor);
            if (ch>=eof || *ch==eol) break;   // already checked number of fields previously above
        }
        if (ch<eof && *ch!=eol) STOP("Internal error: reading colnames did not end on eol");
        if (ch<eof) ch+=eolLen;
        pos=ch;    // now on first data row (row after column names)
    }
    int row1Line = line;
    double tLayout = wallclock();
    
    // *****************************************************************************************************************
    //   Make best guess at column types using 100 rows at 100 points, including the very first, middle and very last row.
    //   At the same time, calc mean and sd of row lengths in sample for very good nrow estimate.
    // *****************************************************************************************************************
    typeOnStack = ncol<10000;
    if (typeOnStack) type = (signed char *)alloca(ncol*sizeof(int8_t));
    else             type = (signed char *)malloc(ncol*sizeof(int8_t));
    // (...?alloca:malloc)(...) doesn't compile as alloca is special.
    
    // 9.8KB is for sure fine on stack. Almost went for 1MB (1 million columns) but decided to be uber safe.
    // sizeof(signed char) == 1 checked in init.c. To free or not to free is in cleanup() based on typeOnStack
    if (!type) STOP("Failed to allocate %dx%d bytes for type: %s", ncol, sizeof(int8_t), strerror(errno));
    for (int i=0; i<ncol; i++) type[i] = 1; // lowest enum is 1 (CT_BOOL8 at the time of writing). 0==CT_DROP
    
    size_t jump0size=(size_t)(firstJumpEnd-pos);  // the size in bytes of the first JUMPLINES from the start (jump point 0)
    int nJumps = 0;
    // how many places in the file to jump to and test types there (the very end is added as 11th or 101th)
    // not too many though so as not to slow down wide files; e.g. 10,000 columns.  But for such large files (50GB) it is
    // worth spending a few extra seconds sampling 10,000 rows to decrease a chance of costly reread even further.
    if (jump0size>0) {
      if (jump0size*100*2 < (size_t)(eof-pos)) nJumps=100;  // 100 jumps * 100 lines = 10,000 line sample
      else if (jump0size*10*2 < (size_t)(eof-pos)) nJumps=10;
      // *2 to get a good spacing. We don't want overlaps resulting in double counting.
      // nJumps==1 means the whole (small) file will be sampled with one thread
    }
    nJumps++; // the extra sample at the very end (up to eof) is sampled and format checked but not jumped to when reading
    if (args.verbose) {
      DTPRINT("Number of sampling jump points = %d because ",nJumps);
      if (jump0size==0) DTPRINT("jump0size==0\n");
      else DTPRINT("%lld bytes from row 1 to eof / (2 * %lld jump0size) == %d\n",
                   (size_t)(eof-pos), jump0size, (size_t)(eof-pos)/(2*jump0size));
    }

    int sampleLines=0;
    size_t sampleBytes=0;
    double sumLen=0.0, sumLenSq=0.0;
    int minLen=INT32_MAX, maxLen=-1;   // int_max so the first if(thisLen<minLen) is always true; similarly for max
    const char *lastRowEnd=pos;
    for (int j=0; j<nJumps; j++) {
        ch = ( j==0 ? pos : (j==nJumps-1 ? eof-(size_t)(0.5*jump0size) : pos + j*((size_t)(eof-pos)/(nJumps-1))));
        if (j>0 && !nextGoodLine(&ch, ncol))
          STOP("Could not find first good line start after jump point %d when sampling.", j);
        if (ch<lastRowEnd) // otherwise an overlap would lead to double counting and a wrong estimate
          STOP("Internal error: Sampling jump point %d is before the last jump ended", j);
        _Bool bumped = 0;  // did this jump find any different types; to reduce args.verbose output to relevant lines
        const char *thisStart = ch;
        int line = 0;  // line from this jump point
        while(ch<eof && (line<JUMPLINES || j==nJumps-1)) {  // nJumps==1 implies sample all of input to eof; last jump to eof too
            const char *lineStart = ch;
            if (sep==' ') while (ch<eof && *ch==' ') ch++;  // multiple sep=' ' at the lineStart does not mean sep(!)
            skip_white(&ch);  // solely to detect blank lines, otherwise could leave to field processors
            if (ch>=eof || *ch==eol) {
              if (!skipEmptyLines && !fill) break;
              lineStart = ch;  // to avoid 'Line finished early' below and get to the sampleLines++ block at the end of this while
            }
            line++;
            int field=0;
            const char *fieldStart=ch;  // Needed outside loop for error messages below
            while (ch<eof && *ch!=eol && field<ncol) {
                //DTPRINT("<<%.*s>>", STRLIM(ch,20), ch);
                fieldStart=ch;
                while (type[field]<=CT_STRING && !(*fun[type[field]])(&ch,NULL,0)) {
                  ch=fieldStart;
                  if (type[field]<CT_STRING) { type[field]++; bumped=true; }
                  else {
                    // the field could not be read with this quote rule, try again with next one
                    // Trying the next rule will only be successful if the number of fields is consistent with it
                    if (quoteRule<3) {
                      if (args.verbose)
                        DTPRINT("Bumping quote rule from %d to %d due to field %d on line %d of sampling jump %d starting <<%.*s>>\n",
                                 quoteRule, quoteRule+1, field+1, line, j, STRLIM(fieldStart,200), fieldStart);
                      quoteRule++;
                      bumped=true;
                      ch = lineStart;  // Try whole line again, in case it's a hangover from previous field
                      field=0;
                      continue;
                    }
                    STOP("Even quoteRule 3 was insufficient!");
                  }
                }
                //DTPRINT("%d", type[field]);
                if (ch<eof && *ch!=eol) {ch++; field++;}
            }
            if (field<ncol-1 && !fill) {
                if (ch<eof && *ch!=eol) STOP("Internal error: line has finished early but not on an eol or eof (fill=false). Please report as bug.");
                else if (ch>lineStart) STOP("Line has too few fields when detecting types. Use fill=TRUE to pad with NA. Expecting %d fields but found %d: <<%.*s>>", ncol, field+1, STRLIM(lineStart,200), lineStart);
            }
            if (ch<eof) {
                if (*ch!=eol || field>=ncol) {   // the || >=ncol is for when a comma ends the line with eol straight after 
                  if (field!=ncol) STOP("Internal error: Line has too many fields but field(%d)!=ncol(%d)", field, ncol);
                  STOP("Line %d from sampling jump %d starting <<%.*s>> has more than the expected %d fields. " \
                       "Separator %d occurs at position %d which is character %d of the last field: <<%.*s>>. " \
                       "Consider setting 'comment.char=' if there is a trailing comment to be ignored.",
                      line, j, STRLIM(lineStart,10), lineStart, ncol, ncol, (int)(ch-lineStart), (int)(ch-fieldStart),
                      STRLIM(fieldStart,200), fieldStart);
                }
                ch += eolLen;
            } else {
                // if very last field was quoted, check if it was completed with an ending quote ok.
                // not necessarily a problem (especially if we detected no quoting), but we test it and nice to have
                // a warning regardless of quoting rule just incase file has been inadvertently truncated
                // This warning is early at type skipping around stage before reading starts, so user can cancel early
                if (type[ncol-1]==CT_STRING && *fieldStart==quote && *(ch-1)!=quote) {
                  if (quoteRule<2) STOP("Internal error: Last field of last field should select quote rule 2"); 
                  DTWARN("Last field of last line starts with a quote but is not finished with a quote before end of file: <<%.*s>>", 
                          STRLIM(fieldStart, 200), fieldStart);
                }
            }
            //DTPRINT("\n");
            lastRowEnd = ch; // Two reasons:  1) to get the end of the very last good row before whitespace or footer before eof
                             //               2) to check sample jumps don't overlap, otherwise double count and bad estimate
            int thisLineLen = (int)(ch-lineStart);  // ch is now on start of next line so this includes eolLen already
            sampleLines++;
            sumLen += thisLineLen;
            sumLenSq += thisLineLen*thisLineLen;
            if (thisLineLen<minLen) minLen=thisLineLen;
            if (thisLineLen>maxLen) maxLen=thisLineLen;
        }
        sampleBytes += (size_t)(ch-thisStart);
        if (args.verbose && (bumped || j==0 || j==nJumps-1)) {
          DTPRINT("Type codes (jump %03d)    : ",j); printTypes(type, ncol);
          DTPRINT("  Quote rule %d\n", quoteRule);
        }
    }
    while (ch<eof && isspace(*ch)) ch++;
    if (ch<eof) {
      DTWARN("Found the last consistent line but text exists afterwards (discarded): <<%.*s>>", STRLIM(ch,200), ch);
    }
    
    int estnrow=1;
    int allocnrow=1, orig_allocnrow=1;
    double meanLineLen=0;
    if (sampleLines<=1) {
      // column names only are present; e.g. fread("A\n")
    } else {
      meanLineLen = (double)sumLen/sampleLines;
      estnrow = (int)ceil((double)(lastRowEnd-pos)/meanLineLen);  // only used for progress meter and args.verbose line below
      double sd = sqrt( (sumLenSq - (sumLen*sumLen)/sampleLines)/(sampleLines-1) );
      allocnrow = (int)ceil((double)(lastRowEnd-pos)/fmax((meanLineLen-2*sd),minLen));
      orig_allocnrow = allocnrow = umin(umax(allocnrow, ceil(1.1*estnrow)), 2*estnrow);
      // orig_ for when nrows= is passed in. We need the original later to calc initial buffer sizes
      // sd can be very close to 0.0 sometimes, so apply a +10% minimum
      // blank lines have length 1 so for fill=true apply a +100% maximum. It'll be grown if needed.
      if (args.verbose) {
        DTPRINT("=====\n Sampled %d rows (handled \\n inside quoted fields) at %d jump points including middle and very end\n", sampleLines, nJumps);
        DTPRINT(" Bytes from first data row on line %d to the end of last row: %lld\n", row1Line, (size_t)(lastRowEnd-pos));
        DTPRINT(" Line length: mean=%.2f sd=%.2f min=%d max=%d\n", meanLineLen, sd, minLen, maxLen);
        DTPRINT(" Estimated nrow: %lld / %.2f = %d\n", (size_t)(lastRowEnd-pos), meanLineLen, estnrow);
        DTPRINT(" Initial alloc = %d rows (%d + %d%%) using bytes/max(mean-2*sd,min) clamped between [1.1*estn, 2.0*estn]\n",
                 allocnrow, estnrow, (int)(100.0*allocnrow/estnrow-100.0));
      }
      if (nJumps==1) {
        if (args.verbose) DTPRINT(" All rows were sampled since file is small so we know nrow=%d exactly\n", sampleLines);
        estnrow = allocnrow = sampleLines;
      } else {
        if (sampleLines > allocnrow) STOP("Internal error: sampleLines(%d) > allocnrow(%d)", sampleLines, allocnrow);
      }
      if (args.nrowLimit<allocnrow) {
        if (args.verbose) DTPRINT(" Alloc limited to lower nrows=%d passed in.\n", args.nrowLimit);
        estnrow = allocnrow = args.nrowLimit;
      }
      if (args.verbose) DTPRINT("=====\n");
    }
    
    // ********************************************************************************************
    //   Apply colClasses, select, drop and integer64
    // ********************************************************************************************
    ch = pos;
    signed char *oldType = (signed char *)malloc(ncol*sizeof(signed char));
    if (!oldType) STOP("Unable to allocate %d bytes to check user overrides of column types", ncol);
    memcpy(oldType, type, ncol);
    if (!userOverride(type, colNames, colNamesAnchor, ncol)) { // colNames must not be changed but type[] can be
      if (args.verbose) DTPRINT("Cancelled by user. userOverride() returned false.");
      return;
    }
    int ndrop=0, nUserBumped=0;
    int nStringCols = 0, nNonStringCols = 0;
    for (int i=0; i<ncol; i++) {
      if (type[i]==CT_DROP) { ndrop++; continue; }
      if (type[i]<oldType[i])
        STOP("Attempt to override column %d <<%.*s>> of inherent type '%s' down to '%s' which will lose accuracy. " \
             "If this was intended, please coerce to the lower type afterwards. Only overrides to a higher type are permitted.",
             i+1, colNames[i].len, colNamesAnchor+colNames[i].off, typeName[oldType[i]], typeName[type[i]]);
      nUserBumped += type[i]>oldType[i];
      nStringCols += type[i]==CT_STRING;
      nNonStringCols += type[i]!=CT_STRING;
    }
    free(oldType);
    if (args.verbose) {
      DTPRINT("After %d type and %d drop user overrides : ", nUserBumped, ndrop);
      printTypes(type, ncol); DTPRINT("\n");
    }
    double tColType = wallclock();
    
    // ********************************************************************************************
    //   Allocate the result columns
    // ********************************************************************************************
    if (args.verbose) DTPRINT("Allocating %d column slots (%d - %d dropped)\n", ncol-ndrop, ncol, ndrop);
    double ansGB = allocateDT(type, ncol, ndrop, allocnrow);
    double tAlloc = wallclock();
    
    // ********************************************************************************************
    //   madvise sequential
    // ********************************************************************************************
    // Read ahead and drop behind each point as they move through (assuming it's on a per thread basis).
    // Considered it but when processing string columns the buffers point to offsets in the mmp'd pages
    // which are revisited when writing the finished buffer to DT. So, it isn't sequential.
    // if (fnam!=NULL) {
    //   #ifdef MADV_SEQUENTIAL  // not on Windows. PrefetchVirtualMemory from Windows 8+ ?  
    //   int ret = madvise((void *)mmp, (size_t)fileSize, MADV_SEQUENTIAL); 
    //   #endif
    // }
    
    // ********************************************************************************************
    //   Read the data
    // ********************************************************************************************
    ch = pos;   // back to start of first data row
    int hasPrinted=0;  // the percentage last printed so it prints every 2% without many calls to wallclock()
    _Bool stopTeam=false, firstTime=true;
    int nTypeBump=0, nTypeBumpCols=0;
    double tRead=0, tReread=0, tTot=0;
    char *typeBumpMsg=NULL;  size_t typeBumpMsgSize=0;
    #define stopErrSize 1000
    char stopErr[stopErrSize+1]="";  // must be compile time size: the message is generated and we can't free before STOP
    int ansi=0;   // the current row number in ans that we are writing to
    const char *prevThreadEnd = pos;  // the position after the last line the last thread processed (for checking)
    size_t workSize=0;
    int initialBuffRows=0, buffGrown=0;
    
    size_t chunkBytes = umax(1000*maxLen, 1/*MB*/ *1024*1024);  // 1000 was 5
    // Decides number of jumps and size of buffers; chunkBytes is the distance between each jump point
    // For the 44GB file with 12875 columns, the max line len is 108,497. As each column has its own buffer per thread,
    // that buffer allocation should be at least one page (4k). Hence 1000 rows of the smallest type (4 byte int) is just
    // under 4096 to leave space for R's header + malloc's header. Around 50MB of buffer in this extreme case.
    if (nJumps/*from sampling*/>1 && args.nth>1) {
      // ensure data size is split into same sized chunks (no remainder in last chunk) and a multiple of nth
      nJumps = (int)((size_t)(lastRowEnd-pos)/chunkBytes);  // (int) rounds down
      if (nJumps==0) nJumps=1;
      else if (nJumps>args.nth) nJumps = args.nth*(1+(nJumps-1)/args.nth);
      chunkBytes = (size_t)(lastRowEnd-pos)/nJumps;
    } else nJumps=1;
    int nth = umin(nJumps, args.nth);
    initialBuffRows = umax(orig_allocnrow/nJumps,500);
    // minimum of any malloc is one page (4k). 4096/8 = 512. Use 500 to leave room for malloc's internal header to fit on 1 page.
    // However, chunkBytes MAX should have already been reflected in nJumps, so the 500 doesn't matter really.
    // orig_allocnrow typically 10-20% bigger than estimated final nrow, so the buffers have the same initial overage %.
    // buffers will be grown if i) we observe a lot of out-of-sample short lines
    // or ii) the lines are shorter in the first half of the file than the second half, for example
    
    read:  // we'll return here to reread any columns with out-of-sample type exceptions
    #pragma omp parallel num_threads(nth)
    {
      int me = omp_get_thread_num();
      #pragma omp master
      {
        int nth_actual = omp_get_num_threads();
        if (nth_actual>nth) {
          snprintf(stopErr, stopErrSize, "OpenMP error: omp_get_num_threads()=%d > num_threads(nth=%d) directive",
                                         nth_actual, nth);
          stopTeam=true;
        } else {
          if (nth_actual < nth) {
            DTWARN("Team started with %d threads despite num_threads(nth=%d) directive. Please file an issue on GitHub. This should never happen because nth was already limited by omp_get_max_threads() which should reflect system level limiting. It should still work but using more time and space than is necessary.\n", nth_actual, nth);
            nth = nth_actual;
            // don't do ... if (nth==1) buff=ans;  because nJumps>1 to rewrite to the buffer and we don't want to rewrite to ans
          }
          if (args.verbose) DTPRINT("Reading %d chunks of %.3fMB (%d rows) using %d threads\n",
                                nJumps, (double)chunkBytes/(1024*1024), (int)(chunkBytes/meanLineLen), nth);
        }
      }
      #pragma omp barrier
      void **mybuff = NULL;
      // When nth=1 we could point mybuff directly to ans, were it not for character columns and wanted to keep SET_ out of Field
      // Allocate mybuff here inside parallel region so that OpenMP/OS knows it doesn't need to sync the buffers between threads
      int myBuffRows = initialBuffRows;
      if (!stopTeam) {
        mybuff = (void **)calloc(ncol-ndrop,sizeof(void *));  // not VLA for when ncol>10,000. calloc for free(NULL) later
        // TODO: on stack with alloca
        if (!mybuff) stopTeam=true;
        for (int j=0, resj=-1; !stopTeam && j<ncol; j++) {   // LENGTH(ans) not ncol because LENGTH(ans)<ncol when ndrop>0
          if (type[j] == CT_DROP) continue;
          resj++;
          if (type[j] < 0) continue;  // on the reread there will be -CT_STRING to skip columns already read
          size_t size = typeSize[type[j]];
          if (!(mybuff[resj] = (void *)malloc(myBuffRows * size))) stopTeam=true;
          #pragma omp atomic
          workSize += myBuffRows * size;
        }
      }
      #pragma omp barrier
      #pragma omp master
      if (stopTeam) {
        //TODO set flag for STOP("Unable to allocate thread buffers");   // should never happen as buffers are small
        // free of any that worked is done after for drops through
      }
      #pragma omp for ordered schedule(dynamic)
      for (int jump=0; jump<nJumps; jump++) {
        if (stopTeam) continue;
        int myStopReason=0;
        int j=-1;  // in this scope to be used in error message in ordered
        const char *ch = pos+jump*chunkBytes;
        const char *nextJump = jump<nJumps-1 ? ch+chunkBytes : lastRowEnd-eolLen;
        int buffi=0;  // the row read so far from this jump point. We don't know how many rows exactly this will be yet
        if (jump>0 && !nextGoodLine(&ch, ncol)) {
          stopTeam=true;
          DTPRINT("no good line could be found from jump point %d\n",jump); // TODO: change to stopErr
          continue;
        }
        const char *thisThreadStart=ch;
        const char *lineStart=ch;
        nextJump+=eolLen;  // for when nextJump happens to fall exactly on a line start (or on eol2 on Windows). The
        //                 // next thread will start one line later because nextGoodLine() starts by finding next eol
        //                 // Easier to imagine eolLen==1 and ch<=nextJump in the while condition
        while (ch<nextJump && buffi<args.nrowLimit) {
        // buffi<nrowLimit doesn't make sense when nth>1 since it's always true then (buffi is within buffer while
        // nrowLimit applies to final ans). It's only there for when nth=1 and nrows= is provided (e.g. tests 1558.1
        // and 1558.3). In that case we know we can stop when we've read the required number of rows. Otherwise it
        // would grow ans wastefully.
          if (buffi==myBuffRows) {
            // buffer full due to unusually short lines in this chunk vs the sample; e.g. #2070
            myBuffRows*=1.5;
            for (int j=0, resj=-1; !stopTeam && j<ncol; j++) {
              if (type[j] == CT_DROP) continue;
              resj++;
              if (type[j] < 0) continue;
              size_t size = typeSize[type[j]];
              if (!(mybuff[resj] = (void *)realloc(mybuff[resj], myBuffRows * size))) stopTeam=true;
            }
            if (stopTeam) break;
            #pragma omp atomic
            buffGrown++;  // just for verbose message afterwards
          }
          lineStart = ch;  // for error message
          if (sep==' ') while (ch<eof && *ch==' ') ch++;  // multiple sep=' ' at the lineStart does not mean sep(!)
          skip_white(&ch);  // solely for blank lines otherwise could leave to field processors which handle leading white
          if (ch>=eof || *ch==eol) {
            if (skipEmptyLines) { ch+=eolLen; continue; }
            else if (!fill) { myStopReason=1; break; }
            // in ordered we'll make the error message when we know the line number and stopTeam then 
          }
          j=0;
          int resj=0;
          while (j<ncol) {
            // DTPRINT("Field %d: '%.10s' as type %d\n", j+1, ch, type[j]);
            const char *fieldStart = ch;
            int8_t oldType = type[j];   // fetch shared type once. Cannot read half-written byte.
            int8_t thisType = oldType;  // to know if it was bumped in (rare) out-of-sample type exceptions
            void *buffcol = thisType>0 ? mybuff[resj] : NULL;
            while (!fun[abs(thisType)](&ch, buffcol, buffi)) {
              // normally returns success(1) and buffcol[buffi] is assigned inside *fun.
              buffcol = NULL;   // on next call to *fun don't write the result to the column, as this col now in type exception
              thisType = thisType<0 ? thisType-1 : -thisType-1;
              // guess is insufficient out-of-sample, type is changed to negative sign and then bumped. Continue to
              // check that the new type is sufficient for the rest of the column to be sure a single re-read will work.
              ch = fieldStart;
            }
            if (oldType == CT_STRING) ((lenOff *)buffcol)[buffi].off += (size_t)(fieldStart-thisThreadStart);
            else if (thisType != oldType) {  // rare out-of-sample type exception
              #pragma omp critical
              {
                oldType = type[j];  // fetch shared value again in case another thread bumped it while I was waiting.
                // Can't PRINT because we're likely not master. So accumulate message and print afterwards.
                // We don't know row number yet, as we jumped here in parallel; have a good guess at the range of row number though.
                if (thisType < oldType) {   // thisType<0 (type-exception)
                  char temp[1001];
                  int len = snprintf(temp, 1000,
                    "Column %d (\"%.*s\") bumped from '%s' to '%s' due to <<%.*s>> ",
                    j+1, colNames[j].len, colNamesAnchor + colNames[j].off,
                    typeName[abs(oldType)], typeName[abs(thisType)],
                    (int)(ch-fieldStart), fieldStart);
                  if (nth==1) len += snprintf(temp+len, 1000-len, "on row %d\n", buffi);
                  else len += snprintf(temp+len, 1000-len, "somewhere between row %d and row %d\n", ansi, ansi+nth*initialBuffRows);
                  typeBumpMsg = realloc(typeBumpMsg, typeBumpMsgSize+len+1);
                  strcpy(typeBumpMsg+typeBumpMsgSize, temp);
                  typeBumpMsgSize += len;
                  nTypeBump++;
                  if (oldType>0) nTypeBumpCols++;
                  type[j] = thisType;
                } // else other thread bumped to a (negative) higher or equal type, so do nothing
              }
            }
            resj += (thisType!=CT_DROP);
            j++;
            if (ch>=eof || *ch==eol) break;
            ch++;
          }
          if (j<ncol)  {
            // not enough columns observed
            if (!fill) { myStopReason = 2; break; }
            while (j<ncol) {
              void *buffcol = mybuff[resj];
              switch (type[j]) {
              case CT_BOOL8:
                ((int8_t *)buffcol)[buffi] = NA_BOOL8;
                break;
              case CT_INT32:
                ((int32_t *)buffcol)[buffi] = NA_INT32;
                break;
              case CT_INT64:
                ((int64_t *)buffcol)[buffi] = NA_INT64;
                break;
              case CT_FLOAT64:
                ((double *)buffcol)[buffi] = NA_FLOAT64;
                break;
              case CT_STRING:                
                ((lenOff *)buffcol)[buffi].len = blank_is_a_NAstring ? INT8_MIN : 0;
                ((lenOff *)buffcol)[buffi].off = 0;
                break;
              default:
                break;
              }
              resj += (type[j++]!=CT_DROP);
            }
          }
          if (ch<eof && *ch!=eol) { myStopReason = 3; break; }
          ch+=eolLen;
          buffi++;
        }
        
        int myansi=0;  // which row in the final result to write my buffer to
        int howMany=0; // how many to write. 
        #pragma omp ordered
        {
          if (!myStopReason && !stopTeam) {
            // Normal branch
            if (prevThreadEnd != thisThreadStart) {
              snprintf(stopErr, stopErrSize,
                "Jump %d did not end exactly where jump %d found its first good line start: "
                "prevEnd(%p)<<%.*s>> != thisStart(prevEnd%+d)<<%.*s>>",
                jump-1, jump, prevThreadEnd, STRLIM(prevThreadEnd,50), prevThreadEnd,
                (int)(thisThreadStart-prevThreadEnd), STRLIM(thisThreadStart,50), thisThreadStart);
              stopTeam=true;
            } else {
              myansi = ansi;  // fetch shared ansi -- where to write my results to the answer.
              prevThreadEnd = ch; // tell the next thread where I finished so it can check it started exactly there
              if (myansi<args.nrowLimit) {
                // Normal branch
                howMany = umin(buffi, args.nrowLimit-myansi);
                ansi += howMany;  // update shared ansi to tell the next thread which row I am going to finish on
                // The next thread can now go ahead and copy its results to ans at the same time as I copy my results to ans
              } else {
                stopTeam=true;
                // nrowLimit was supplied and these required rows were handled by previous jumps as I was running
              }
            }
          } else if (!stopTeam) {
            // stopping here in ordered enables to stop on the first error in the file. we now know the row number since
            // previous jumps have already passed through here
            int line=ansi+buffi+row1Line;
            switch(myStopReason) {
            case 1:
              snprintf(stopErr, stopErrSize,
              "Line %d is empty. It is outside the sample rows. " \
              "Set fill=true to treat it as an NA row, or blank.lines.skip=true to skip it", line);
              // TODO - include a few (numbered) lines before and after in the message
              break;
            case 2:
              snprintf(stopErr, stopErrSize,
              "Expecting %d cols but line %d contains only %d cols (sep='%c'). " \
              "Consider fill=true. <<%.*s>>",
              ncol, line, j, sep, STRLIM(lineStart, 500), lineStart);
              break;
            case 3:
              snprintf(stopErr, stopErrSize,
              "Too many fields on line %d outside the sample. Read all %d expected columns but more are present. <<%.*s>>",
              line, ncol, STRLIM(lineStart, 500), lineStart);
              break;
            default:
              snprintf(stopErr, stopErrSize, "Internal error: unknown myStopReason %d", myStopReason);
            }
            stopTeam=true;
          }
        } // end ordered
        if (stopTeam) continue;
        
        if (howMany < buffi) {
          // nrows was set by user (a limit of rows to read) and this is the last jump that fills up the required nrows
          stopTeam=true;
          // otherwise this thread would pick up the next jump and read that wastefully before stopping at ordered
        }
        
        // Assign my buffer to ans now while these pages are hot in my core. I've just this millisecond found out
        // which row to put them on (myansi). All threads do this at the same time, as soon as the previous thread
        // knows its number of rows, in ordered above. Up to impl whether it can push its string columns to ans
        // in parallel. It can have an orphan critical directive if it needs to.
        pushBuffer(type, ncol, mybuff, thisThreadStart, nStringCols, nNonStringCols, howMany, myansi);
        
        if (me==0 && (hasPrinted || (args.showProgress && jump/nth==4 && 
                                    ((double)nJumps/(nth*4)-1.0)*(wallclock()-tAlloc)>3.0))) {
          // Important for thread safety inside progess() that this is called not just from critical but that
          // it's the master thread too, hence me==0.
          // Jump 0 might not be assigned to thread 0; jump/nth==4 to wait for 4 waves to complete then decide once.
          int p = (int)(100.0*(jump+1)/nJumps);
          if (p>=hasPrinted) {
            // ETA TODO. Ok to call wallclock() now.
            progress(p, /*eta*/0);
            hasPrinted = p+2;  // update every 2%
          }
        }
      }
      // Each thread to free its buffers. In event of any alloc errors, this will free the parts that worked
      if (mybuff) for (int j=0; j<ncol-ndrop; j++) { free(mybuff[j]); mybuff[j]=NULL; }
      free(mybuff); mybuff=NULL;
    }  // end OpenMP parallel
    if (firstTime) {
      tReread = tRead = wallclock();
      tTot = tRead-t0;
      if (hasPrinted || args.verbose) {
        DTPRINT("\rRead %d rows x %d columns from %.3fGB file in ", ansi, ncol-ndrop, 1.0*fileSize/(1024*1024*1024));
        DTPRINT("%02d:%06.3f ", (int)tTot/60, fmod(tTot,60.0));
        DTPRINT("wall clock time (can be slowed down by any other open apps even if seemingly idle)\n");
        // since parallel, clock() cycles is parallel too: so wall clock will have to do
      }
      // not-bumped columns are assigned type -CT_STRING in the rerun, so we have to count types now
      if (args.verbose) {
        DTPRINT("Thread buffers were grown %d times (if all %d threads each grew once, this figure would be %d)\n",
                 buffGrown, nth, nth);
        int typeCounts[NUMTYPE];
        for (int i=0; i<NUMTYPE; i++) typeCounts[i] = 0;
        for (int i=0; i<ncol; i++) typeCounts[ abs(type[i]) ]++;
        DTPRINT("Final type counts\n");
        for (int i=0; i<NUMTYPE; i++) DTPRINT("%10d : %-9s\n", typeCounts[i], typeName[i]);
        DTPRINT("nStringCols=%d, nNonStringCols=%d\n", nStringCols, nNonStringCols); 
      }
      if (nTypeBump) {
        if (hasPrinted || args.verbose) DTPRINT("Rereading %d columns due to out-of-sample type exceptions.\n", nTypeBumpCols);
        if (args.verbose) DTPRINT(typeBumpMsg);
        // TODO - construct and output the copy and pastable colClasses argument to use to avoid the reread in future.
        free(typeBumpMsg);
      }
    } else {
      tReread = wallclock();
      tTot = tReread-t0;
      if (hasPrinted || args.verbose) {
        DTPRINT("\rReread %d rows x %d columns in ", ansi, nTypeBumpCols);
        DTPRINT("%02d:%06.3f\n", (int)(tReread-tRead)/60, fmod(tReread-tRead,60.0));
      }
    }
    if (stopTeam && stopErr[0]!='\0') STOP(stopErr); // else nrowLimit applied and stopped early normally
    if (ansi > allocnrow) {
      if (args.nrowLimit>allocnrow) STOP("Internal error: ansi(%d)>allocnrow(%d) but nrows=%d (not limited)",
                                         ansi, allocnrow, args.nrowLimit);
      // for the last jump that fills nrow limit, then ansi is +=buffi which is >allocnrow and correct
    } else if (ansi == allocnrow) {
      if (args.verbose) DTPRINT("Read %d rows. Exactly what was estimated and allocated up front\n", ansi);
    } else {
      setFinalNrow(ansi);
      allocnrow = ansi;
    }
    if (firstTime && nTypeBump) {
      nStringCols = nNonStringCols = 0;
      for (int j=0, resj=-1; j<ncol; j++) {
        if (type[j] == CT_DROP) continue;
        resj++;
        if (type[j]<0) {
          // just for the bumped columns ...
          int newType = type[j] *= -1;   // final type for this column
          reallocColType(resj, newType);
          if (newType==CT_STRING) nStringCols++; else nNonStringCols++;
        } else if (type[j]>=1) {
          // we'll skip over non-bumped columns in the rerun, whilst still incrementing resi (hence not CT_DROP)
          // not -type[i] either because that would reprocess the contents of not-bumped columns wastefully
          type[j] = -CT_STRING;
        }
      }
      // reread from the beginning
      ansi = 0;
      prevThreadEnd = ch = pos;
      firstTime = false;
      goto read;
    }
    if (args.verbose) {
      DTPRINT("=============================\n");
      if (tTot<0.000001) tTot=0.000001;  // to avoid nan% output in some trivially small tests where tot==0.000s
      DTPRINT("%8.3fs (%3.0f%%) Memory map\n", tMap-t0, 100.0*(tMap-t0)/tTot);
      DTPRINT("%8.3fs (%3.0f%%) sep, ncol and header detection\n", tLayout-tMap, 100.0*(tLayout-tMap)/tTot);
      DTPRINT("%8.3fs (%3.0f%%) Column type detection using %d sample rows\n", tColType-tLayout, 100.0*(tColType-tLayout)/tTot, sampleLines);
      DTPRINT("%8.3fs (%3.0f%%) Allocation of %d rows x %d cols (%.3fGB) plus %.3fGB of temporary buffers\n", tAlloc-tColType, 100.0*(tAlloc-tColType)/tTot, allocnrow, ncol, ansGB, (double)workSize/(1024*1024*1024));
      DTPRINT("%8.3fs (%3.0f%%) Reading data\n", tRead-tAlloc, 100.0*(tRead-tAlloc)/tTot);
      DTPRINT("%8.3fs (%3.0f%%) Rereading %d columns due to out-of-sample type exceptions\n", tReread-tRead, 100.0*(tReread-tRead)/tTot, nTypeBumpCols);
      DTPRINT("%8.3fs        Total\n", tTot);
    }
    cleanup();
}

