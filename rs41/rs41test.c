
/*
 * radiosondes RS41-SG(P)
 * author: zilog80
 * usage:
 *     gcc rs41test.c -lm -o rs41test
 *     ./rs41test [options] audio.wav
 *       options:
 *               -v, --verbose
 *               -r, --raw
 *     ./rs41test audio.wav
 *     ./rs41test -r audio.wav | less -S
 *     ./rs41test -v audio.wav 1> /dev/null
 *     ./rs41test -v audio.wav 1> pos.txt
 *     ./rs41test -v audio.wav 2> cal.txt
 *     ./rs41test -v audio.wav 2>&1 >/dev/null | grep 0x00
 *     ./rs41test < audio.wav
 *     sox -t oss /dev/dsp -t wav - 2>/dev/null | ./rs41test
 *     sox -t oss /dev/dsp -t wav - lowpass 3000 2>/dev/null | ./rs41test
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef CYGWIN
  #include <fcntl.h>  // cygwin: _setmode()
  #include <io.h>
#endif

typedef unsigned char ui8_t;

typedef struct {
    int frnr;
    char id[9];
    int week; int gpssec;
    int jahr; int monat; int tag;
    int wday;
    int std; int min; int sek;
    double lat; double lon; double h;
    double vN; double vE; double vU;
    double vH; double vD; double vD2;
} gpx_t;

gpx_t gpx;

int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 0,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    option_res = 0,      // genauere Bitmessung
    option_auto = 0,
    wavloaded = 0;


#define HEADOFS 16 // HEADOFS+HEADLEN <= 64
#define HEADLEN 48 // HEADOFS+HEADLEN mod 8 = 0
#define FRAMESTART ((HEADOFS+HEADLEN)/8)

/*               10      B6      CA      11      22      96      12      F8      */      
char header[] = "0000100001101101010100111000100001000100011010010100100000011111";
char buf[HEADLEN+1] = "x";
int bufpos = -1;

#define AUX_LEN   (260)
#define FRAME_LEN (320+AUX_LEN)
ui8_t frame[FRAME_LEN] = { 0x10, 0xB6, 0xCA, 0x11, 0x22, 0x96, 0x12, 0xF8};


#define MASK_LEN 64
ui8_t mask[MASK_LEN] = { 0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98, 
                         0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
                         0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1,
                         0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
                         0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C,
                         0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
                         0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23,
                         0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};
/* LFSR: ab i=8 (mod 64):
 * m[16+i] = m[i] ^ m[i+2] ^ m[i+4] ^ m[i+6]
 * ________________3205590EF944C6262160C2EA795D6DA15469470CDCE85CF1
 * F776827F0799A22C937C3063F5102E61D0BCB4B606AAF423786E3BAEBF7B4CC196833E51B1490898
 */

/* ------------------------------------------------------------------------------------ */

#define BAUD_RATE 4800

int sample_rate = 0, bits_sample = 0, channels = 0;
float samples_per_bit = 0;

int findstr(char *buff, char *str, int pos) {
    int i;
    for (i = 0; i < 4; i++) {
        if (buff[(pos+i)%4] != str[i]) break;
    }
    return i;
}

int read_wav_header(FILE *fp) {
    char txt[4+1] = "\0\0\0\0";
    unsigned char dat[4];
    int byte, p=0;

    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "RIFF", 4)) return -1;
    if (fread(txt, 1, 4, fp) < 4) return -1;
    // pos_WAVE = 8L
    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "WAVE", 4)) return -1;
    // pos_fmt = 12L
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "fmt ", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;

    if (fread(dat, 1, 2, fp) < 2) return -1;
    channels = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    memcpy(&sample_rate, dat, 4); //sample_rate = dat[0]|(dat[1]<<8)|(dat[2]<<16)|(dat[3]<<24);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;
    //byte = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 2, fp) < 2) return -1;
    bits_sample = dat[0] + (dat[1] << 8);

    // pos_dat = 36L + info
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "data", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;


    fprintf(stderr, "sample_rate: %d\n", sample_rate);
    fprintf(stderr, "bits       : %d\n", bits_sample);
    fprintf(stderr, "channels   : %d\n", channels);

    if ((bits_sample != 8) && (bits_sample != 16)) return -1;

    samples_per_bit = sample_rate/(float)BAUD_RATE;

    fprintf(stderr, "samples/bit: %.2f\n", samples_per_bit);

    return 0;
}


#define EOF_INT  0x1000000

int read_signed_sample(FILE *fp) {  // int = i32_t
    int byte, i, ret;         //  EOF -> 0x1000000

    for (i = 0; i < channels; i++) {
                           // i = 0: links bzw. mono
        byte = fgetc(fp);
        if (byte == EOF) return EOF_INT;
        if (i == 0) ret = byte;
    
        if (bits_sample == 16) {
            byte = fgetc(fp);
            if (byte == EOF) return EOF_INT;
            if (i == 0) ret +=  byte << 8;
        }

    }

    if (bits_sample ==  8) return ret-128;   // 8bit: 00..FF, centerpoint 0x80=128
    if (bits_sample == 16) return (short)ret;

    return ret;
}

int par=1, par_alt=1;
unsigned long sample_count = 0;

int read_bits_fsk(FILE *fp, int *bit, int *len) {
    static int sample;
    int n, y0;
    float l, x1;
    static float x0;

    n = 0;
    do{
        y0 = sample;
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        sample_count++;
        par_alt = par;
        par =  (sample >= 0) ? 1 : -1;     // 8bit: 0..127,128..255 (-128..-1,0..127)
        n++;
    } while (par*par_alt > 0);

    if (!option_res) l = (float)n / samples_per_bit;
    else {                                 // genauere Bitlaengen-Messung
        x1 = sample/(float)(sample-y0);    // hilft bei niedriger sample rate
        l = (n+x0-x1) / samples_per_bit;   // meist mehr frames (nicht immer)
        x0 = x1;
    }

    *len = (int)(l+0.5);

    if (!option_inv) *bit = (1+par_alt)/2;  // oben 1, unten -1
    else             *bit = (1-par_alt)/2;  // sdr#<rev1381?, invers: unten 1, oben -1

    /* Y-offset ? */

    return 0;
}

/* ------------------------------------------------------------------------------------ */


int bits2byte(char bits[]) {
    int i, byteval=0, d=1;
    for (i = 0; i < 8; i++) {     // little endian
    /* for (i = 7; i >= 0; i--) { // big endian */
        if      (bits[i] == 1)  byteval += d;
        else if (bits[i] == 0)  byteval += 0;
        else return 0x100;
        d <<= 1;      
    }
    return byteval;
}


void inc_bufpos() {
  bufpos = (bufpos+1) % HEADLEN;
}

char cb_inv(char c) {
    if (c == '0') return '1';
    if (c == '1') return '0';
    return c;
}

int compare2() {
    int i, j;

    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != header[HEADOFS+HEADLEN-1-i]) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return 1;

    if (option_auto) {
    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != cb_inv(header[HEADOFS+HEADLEN-1-i])) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return -1;
    }

    return 0;

}

ui8_t xorbyte(int pos) {
    return  frame[pos] ^ mask[pos % MASK_LEN];
}


/* ------------------------------------------------------------------------------------ */
/*
 * Convert GPS Week and Seconds to Modified Julian Day.
 * - Adapted from sci.astro FAQ.
 * - Ignores UTC leap seconds.
 */
void Gps2Date(long GpsWeek, long GpsSeconds, int *Year, int *Month, int *Day) {

    long GpsDays, Mjd;
    long J, C, Y, M;

    GpsDays = GpsWeek * 7 + (GpsSeconds / 86400);
    Mjd = 44244 + GpsDays;

    J = Mjd + 2468570;
    C = 4 * J / 146097;
    J = J - (146097 * C + 3) / 4;
    Y = 4000 * (J + 1) / 1461001;
    J = J - 1461 * Y / 4 + 31;
    M = 80 * J / 2447;
    *Day = J - 2447 * M / 80;
    J = M / 11;
    *Month = M + 2 - (12 * J);
    *Year = 100 * (C - 49) + Y + J;
}
/* ------------------------------------------------------------------------------------ */

/*
SubHeader, 2byte
0x039: 7928  FrameNumber+SondeID
0x050: 0732  CalFrames 0x00..0x32
0x093: 7C1E  GPS Week + TOW
0x112: 7B15  ECEF (X,Y,Z) Coordinates
*/

#define pos_FrameNb   0x03B  // 2 byte
#define pos_SondeID   0x03D  // 8 byte
#define pos_CalData   0x052  // 1 byte, counter 0x00..0x32
#define pos_Calfreq   0x055  // 2 byte, calfr 0x00
#define pos_Calburst  0x05E  // 1 byte, calfr 0x02
// ? #define pos_Caltimer  0x05A  // 2 byte, calfr 0x02 ?
#define pos_CalRSTyp  0x05B  // 8 byte, calfr 0x21 (+2 byte in 0x22?)
        // weitere chars in calfr 0x22/0x23; weitere ID
#define pos_GPSweek   0x095  // 2 byte
#define pos_GPSTOW    0x097  // 4 byte
#define pos_GPSecefX  0x114  // 4 byte
#define pos_GPSecefY  0x118  // 4 byte
#define pos_GPSecefZ  0x11C  // 4 byte
#define pos_GPSecefV  0x120  // 3*2 byte


#define pos_Hframe 0x039
#define HEAD_frame 0x1713  // ^0x6E3B=0x7928

#define pos_Htow   0x093
#define HEAD_tow   0x9667  // ^0xEA79=0x7C1E

#define pos_Hkoord 0x112
#define HEAD_koord 0xB9FF  // ^0xC2EA=0x7B15

unsigned shiftLeft(int pos) {
    unsigned tmp;
    tmp = (frame[pos+1]<<8) + frame[pos];
    tmp = (tmp >> 1) & 0xFF;
    return tmp;
}
int shiftRight(int pos) {
    unsigned tmp;
    tmp = (frame[pos]<<8) + frame[pos-1];
    tmp = (tmp >> 7) & 0xFF;
    return tmp;
}

void shiftFrame(int pos, int shift) {
    unsigned byte, byte1, byte2;

    if (shift > 0) {
        while (pos < FRAME_LEN) {
            byte = shiftLeft(pos);
            frame[pos] = byte;
            pos++;
        }
    }
    if (shift < 0) {
        byte1 = shiftRight(pos);
        pos++;
        while (pos < FRAME_LEN) {
            byte2 = shiftRight(pos);
            frame[pos-1] = byte1;
            byte1 = byte2;
            pos++;
        }
    }

}

int getShift(int pos, unsigned head) {
    unsigned byte;
    int shift = 0;

    byte = (frame[pos]<<8) + frame[pos+1];
    // fprintf(stdout, "0x%04X ", byte );  // HEAD_frame ^ 0x6E38 == 0x7928 ? 
    if (byte != head) {
        byte = (shiftLeft(pos)<<8) + shiftLeft(pos+1);
        //fprintf(stdout, " %04X", byte);
        if (byte == HEAD_frame) shift = 1;
        else {
            byte = (shiftRight(pos)<<8) + shiftRight(pos+1);
            if (byte == head) shift = -1;
        }
        if (!shift) return 0x100;
        //printf("shift:%2d ", shift);
    }
    return shift;
}

int get_FrameNb() {
    int i;
    unsigned byte;
    ui8_t frnr_bytes[2];
    int frnr;
/*  int shift = 0;

    shift = getShift(pos_Hframe, HEAD_frame);
    if (shift == 0x100) return 0x100;
    //printf("shift:%2d ", shift);
    shiftFrame(pos_Hframe, shift);
*/
    for (i = 0; i < 2; i++) {
        byte = xorbyte(pos_FrameNb + i);
        frnr_bytes[i] = byte;
    }

    frnr = frnr_bytes[0] + (frnr_bytes[1] << 8);
    gpx.frnr = frnr;

    return 0;
}

int get_SondeID() {
    int i;
    unsigned byte;
    ui8_t sondeid_bytes[8];

    for (i = 0; i < 8; i++) {
        byte = xorbyte(pos_SondeID + i);
        if ((byte < 0x20) || (byte > 0x7E)) return -1;
        sondeid_bytes[i] = byte;
    }

    for (i = 0; i < 8; i++) {
        gpx.id[i] = sondeid_bytes[i];
    }
    gpx.id[9] = '\0';

    return 0;
}

int get_GPSweek() {
    int i;
    unsigned byte;
    ui8_t gpsweek_bytes[2];
    int gpsweek;
/*  int shift = 0;

    shift = getShift(pos_Htow, HEAD_tow);
    if (shift == 0x100) return 0x100;
    //printf("shift:%2d ", shift);
    shiftFrame(pos_Htow, shift);
*/
    for (i = 0; i < 2; i++) {
        byte = xorbyte(pos_GPSweek + i);
        gpsweek_bytes[i] = byte;
    }

    gpsweek = gpsweek_bytes[0] + (gpsweek_bytes[1] << 8);
    if (gpsweek < 0) { gpx.week = -1; return -1; }
    gpx.week = gpsweek;

    return 0;
}

char weekday[7][3] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

int get_GPStime() {
    int i;
    unsigned byte;
    ui8_t gpstime_bytes[4];
    int gpstime = 0, // 32bit
        day;

    for (i = 0; i < 4; i++) {
        byte = xorbyte(pos_GPSTOW + i);
        gpstime_bytes[i] = byte;
    }

    memcpy(&gpstime, gpstime_bytes, 4);
    //ms = gpstime % 1000;
    gpstime /= 1000;

    gpx.gpssec = gpstime;

    day = gpstime / (24 * 3600);
    gpstime %= (24*3600);

    if ((day < 0) || (day > 6)) return -1;
    gpx.wday = day;
    gpx.std = gpstime / 3600;
    gpx.min = (gpstime % 3600) / 60;
    gpx.sek = gpstime % 60;

    return 0;
}


#define EARTH_a  6378137.0
#define EARTH_b  6356752.31424518
#define EARTH_a2_b2  (EARTH_a*EARTH_a - EARTH_b*EARTH_b)

double a = EARTH_a,
       b = EARTH_b,
       a_b = EARTH_a2_b2,
       e2  = EARTH_a2_b2 / (EARTH_a*EARTH_a),
       ee2 = EARTH_a2_b2 / (EARTH_b*EARTH_b);

void ecef2elli(double X[], double *lat, double *lon, double *h) {
    double phi, lam, R, p, t;

    lam = atan2( X[1] , X[0] );

    p = sqrt( X[0]*X[0] + X[1]*X[1] );
    t = atan2( X[2]*a , p*b );
    
    phi = atan2( X[2] + ee2 * b * sin(t)*sin(t)*sin(t) ,
                 p - e2 * a * cos(t)*cos(t)*cos(t) );

    R = a / sqrt( 1 - e2*sin(phi)*sin(phi) );
    *h = p / cos(phi) - R;
    
    *lat = phi*180/M_PI;
    *lon = lam*180/M_PI;
}

int get_GPSkoord() {
    int i, k;
    unsigned byte;
    ui8_t XYZ_bytes[4];
    int XYZ; // 32bit
    double X[3], lat, lon, h;
    int shift = 0;

    byte = (frame[pos_Hkoord]<<8) + frame[pos_Hkoord+1];
    /* fprintf(stdout, "0x%04X ", byte );  // ^ 0xC2EA == 0x7B15 ? */
    if (byte != HEAD_koord) {
        byte = (shiftLeft(pos_Hkoord)<<8) + shiftLeft(pos_Hkoord+1);
        if (byte == HEAD_koord) shift = 1;
        else {
            byte = (shiftRight(pos_Hkoord)<<8) + shiftRight(pos_Hkoord+1);
            if (byte == HEAD_koord) shift = -1;
        }
        if (!shift) return 0x100;
        //printf("shift:%2d ", shift);
    }

    for (k = 0; k < 3; k++) {

        for (i = 0; i < 4; i++) {
            if      (shift > 0) byte = shiftLeft(pos_GPSecefX + 4*k + i);
            else if (shift < 0) byte = shiftRight(pos_GPSecefX + 4*k + i);
            else                byte = frame[pos_GPSecefX + 4*k + i];
            byte = byte ^ mask[(pos_GPSecefX + 4*k + i) % MASK_LEN];
            XYZ_bytes[i] = byte;
        }

        memcpy(&XYZ, XYZ_bytes, 4);
        X[k] = XYZ / 100.0;

    }

    ecef2elli(X, &lat, &lon, &h);
    gpx.lat = lat;
    gpx.lon = lon;
    gpx.h = h;
    if ((h < -1000) || (h > 80000)) return -1;

    return 0;
}

int get_GPSvel() {
    int i, k;
    unsigned byte;
    ui8_t gpsVel_bytes[2];
    short vel16;
    short V[3];
    double vx, vy, vz;
    double phi, lam, alpha, dir;

    for (k = 0; k < 3; k++) {
        for (i = 0; i < 2; i++) {
            byte = frame[pos_GPSecefV + 2*k + i] ^ mask[(pos_GPSecefV + 2*k + i) % MASK_LEN];
            gpsVel_bytes[i] = byte & 0xFF;
        }
        vel16 = gpsVel_bytes[0] | gpsVel_bytes[1] << 8;
        V[k] = vel16;
    }

    // ECEF-Velocities
    vx = V[0] / 1e2;
    vy = V[1] / 1e2;
    vz = V[2] / 1e2;

    // ECEF-Vel -> NorthEastUp
    phi = gpx.lat*M_PI/180.0;
    lam = gpx.lon*M_PI/180.0;
    gpx.vN = -vx*sin(phi)*cos(lam) - vy*sin(phi)*sin(lam) + vz*cos(phi);
    gpx.vE = -vx*sin(lam) + vy*cos(lam);
    gpx.vU =  vx*cos(phi)*cos(lam) + vy*cos(phi)*sin(lam) + vz*sin(phi);

    // NEU -> HorDirVer
    gpx.vH = sqrt(gpx.vN*gpx.vN+gpx.vE*gpx.vE);
///*
    alpha = atan2(gpx.vN, gpx.vE)*180/M_PI;  // ComplexPlane (von x-Achse nach links) - GeoMeteo (von y-Achse nach rechts)
    dir = 90-alpha;                          // z=x+iy= -> i*conj(z)=y+ix=re(i(pi/2-t)), Achsen und Drehsinn vertauscht
    if (dir < 0) dir += 360;                 // atan2(y,x)=atan(y/x)=pi/2-atan(x/y) , atan(1/t) = pi/2 - atan(t)
    gpx.vD2 = dir;
//*/
    dir = atan2(gpx.vE, gpx.vN) * 180 / M_PI;
    if (dir < 0) dir += 360;
    gpx.vD = dir;

    return 0;
}

int get_Cal() {
    int i;
    unsigned byte;
    ui8_t calfr = 0;
    ui8_t burst = 0;
    int freq = 0, f0 = 0, f1 = 0;
    char sondetyp[9];

    byte = xorbyte(pos_CalData);
    calfr = byte;

    fprintf(stderr, " 0x%02x: ", calfr);
    for (i = 0; i < 16; i++) {
        byte = xorbyte(pos_CalData+1+i);
        fprintf(stderr, "%02x ", byte);
    }

    if (option_verbose == 2) {
        if (calfr == 0x02) {
            byte = xorbyte(pos_Calburst);
            burst = byte;
            fprintf(stderr, ": BK %02X ", burst);
        }
        if (calfr == 0x00) {
            byte = xorbyte(pos_Calfreq) & 0xC0;  // erstmal nur oberste beiden bits
            f0 = (byte * 10) / 64;  // 0x80 -> 1/2, 0x40 -> 1/4 ; dann mal 40
            byte = xorbyte(pos_Calfreq+1);
            f1 = 40 * byte;
            freq = 400000 + f1+f0; // kHz;
            fprintf(stderr, ": fq %d ", freq);
        }
        if (calfr == 0x21) {  // eventuell noch zwei bytes in 0x22
            for (i = 0; i < 9; i++) sondetyp[i] = 0;
            for (i = 0; i < 8; i++) {
                byte = xorbyte(pos_CalRSTyp + i);
                if ((byte >= 0x20) && (byte < 0x7F)) sondetyp[i] = byte;
                else if (byte == 0x00) sondetyp[i] = '\0';
            }
            fprintf(stderr, ": %s ", sondetyp);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------------------ */

int print_position() {
    int err1, err2, err3;

        err1 = err2 = err3 = 0;
        err1 |= get_FrameNb();
        err1 |= get_SondeID();
        err2 |= get_GPSweek();
        err2 |= get_GPStime();
        err3 |= get_GPSkoord();

        if (!err1) {
            if (!err2) Gps2Date(gpx.week, gpx.gpssec, &gpx.jahr, &gpx.monat, &gpx.tag);
            fprintf(stdout, "[%5d] ", gpx.frnr);
            fprintf(stdout, "(%s) ", gpx.id);
            if (!err2) {
                fprintf(stdout, "%s ", weekday[gpx.wday]);
                fprintf(stdout, "%04d-%02d-%02d %02d:%02d:%02d", 
                        gpx.jahr, gpx.monat, gpx.tag, gpx.std, gpx.min, gpx.sek);
                if (option_verbose) fprintf(stdout, " (W %d)", gpx.week);
            }
            if (!err3) {
                fprintf(stdout, " ");
                fprintf(stdout, " lat: %.5f ", gpx.lat);
                fprintf(stdout, " lon: %.5f ", gpx.lon);
                fprintf(stdout, " h: %.2f ", gpx.h);
                if (option_verbose) {
                    get_GPSvel();
                    //fprintf(stdout, "  (%.1f %.1f %.1f) ", gpx.vN, gpx.vE, gpx.vU);
                    fprintf(stdout,"  vH: %.1f  D: %.1f°  vV: %.1f ", gpx.vH, gpx.vD, gpx.vU);
                }
            }
            fprintf(stdout, "\n");  // fflush(stdout);

            if (option_verbose) { 
                fprintf(stderr, "[%5d] ", gpx.frnr);
                if (option_auto) fprintf(stderr, "[%c] ", option_inv?'-':'+');
                get_Cal();
                fprintf(stderr, "\n");
            }
        }

    return err1 | err2 | err3;
}

void print_frame(int len) {
    int i;
    ui8_t byte;

    if (option_raw) {
        for (i = 0; i < len; i++) {
            //byte = frame[i];
            byte = xorbyte(i);
            fprintf(stdout, "%02x", byte);
        }
        fprintf(stdout, "\n");
    }
    else print_position();
}

int main(int argc, char *argv[]) {

    FILE *fp;
    char *fpname;
    char bitbuf[8];
    int bit_count = 0,
        byte_count = FRAMESTART,
        header_found = 0,
        byte, i;
    int bit, len;

#ifdef CYGWIN
    _setmode(fileno(stdin), _O_BINARY);  // _fileno(stdin)
    setbuf(stdout, NULL);
#endif

    fpname = argv[0];
    ++argv;
    while ((*argv) && (!wavloaded)) {
        if      ( (strcmp(*argv, "-h") == 0) || (strcmp(*argv, "--help") == 0) ) {
            fprintf(stderr, "%s [options] audio.wav\n", fpname);
            fprintf(stderr, "  options:\n");
            fprintf(stderr, "       -v, --verbose\n");
            fprintf(stderr, "       -r, --raw\n");
            fprintf(stderr, "       -i, --invert\n");
            return 0;
        }
        else if ( (strcmp(*argv, "-v") == 0) || (strcmp(*argv, "--verbose") == 0) ) {
            option_verbose = 1;
        }
        else if   (strcmp(*argv, "-vv") == 0) { option_verbose = 2; }
        else if   (strcmp(*argv, "--res") == 0) { option_res = 1; }
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--raw") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 0x1;
        }
        else if ( (strcmp(*argv, "--auto") == 0) ) {
            option_auto = 1;
        }
        else {
            fp = fopen(*argv, "rb");
            if (fp == NULL) {
                fprintf(stderr, "%s konnte nicht geoeffnet werden\n", *argv);
                return -1;
            }
            wavloaded = 1;
        }
        ++argv;
    }
    if (!wavloaded) fp = stdin;


    i = read_wav_header(fp);
    if (i) {
        fclose(fp);
        return -1;
    }


    while (!read_bits_fsk(fp, &bit, &len)) {

        if (len == 0) { // reset_frame();
            if (byte_count > FRAME_LEN-AUX_LEN-20) {
                print_frame(byte_count);
                bit_count = 0;
                byte_count = FRAMESTART;
                header_found = 0;
            }
            //inc_bufpos();
            //buf[bufpos] = 'x';
            continue;   // ...
        }

        for (i = 0; i < len; i++) {

            inc_bufpos();
            buf[bufpos] = 0x30 + bit;  // Ascii

            if (!header_found) {
                header_found = compare2();
                if (header_found < 0) option_inv ^= 0x1;
            }
            else {
                bitbuf[bit_count] = bit;
                bit_count++;
            
                if (bit_count == 8) {
                    bit_count = 0;
                    byte = bits2byte(bitbuf);
                    frame[byte_count] = byte;
                    byte_count++;
                    if (byte_count == FRAME_LEN) {
                        print_frame(FRAME_LEN);
                        byte_count = FRAMESTART;
                        header_found = 0;
                    }
                }
            }

        }

    }
    fclose(fp);

    return 0;
}


