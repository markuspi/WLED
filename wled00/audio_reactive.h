/*
 * This file allows you to add own functionality to WLED more easily
 * See: https://github.com/Aircoookie/WLED/wiki/Add-own-functionality
 * EEPROM bytes 2750+ are reserved for your custom use case. (if you extend #define EEPSIZE in const.h)
 * bytes 2400+ are currently ununsed, but might be used for future wled features
 */

#include "wled.h"
#include <driver/i2s.h>

// Comment/Uncomment to toggle usb serial debugging
//#define SR_DEBUG

#ifdef SR_DEBUG
  #define DEBUGSR_PRINT(x) Serial.print(x)
  #define DEBUGSR_PRINTLN(x) Serial.println(x)
  #define DEBUGSR_PRINTF(x...) Serial.printf(x)
#else
  #define DEBUGSR_PRINT(x)
  #define DEBUGSR_PRINTLN(x)
  #define DEBUGSR_PRINTF(x...)
#endif

//#define MIC_LOGGER
//#define MIC_SAMPLING_LOG
//#define FFT_SAMPLING_LOG

// The following 3 lines are for Digital Microphone support
#define I2S_WS 15        // aka LRCL
#define I2S_SD 32        // aka DOUT
#define I2S_SCK 14       // aka BCLK
const i2s_port_t I2S_PORT = I2S_NUM_0;
const int BLOCK_SIZE = 64;

const int SAMPLE_RATE = 10240; // was 16000 for digital mic

TaskHandle_t FFT_Task;

//Use userVar0 and userVar1 (API calls &U0=,&U1=, uint16_t)
#ifndef MIC_PIN
  #define MIC_PIN   36  // Changed to direct pin name - ESP32: 36(ADC1_0) Analog port for microphone
#endif

#ifndef LED_BUILTIN     // Set LED_BUILTIN if it is not defined by Arduino framework
  #define LED_BUILTIN 3
#endif

#define UDP_SYNC_HEADER "00001"

uint8_t maxVol = 6;                             // Reasonable value for constant volume for 'peak detector', as it won't always trigger
uint8_t targetAgc = 60;                         // This is our setPoint at 20% of max for the adjusted output
uint8_t myVals[32];                             // Used to store a pile of samples as WLED frame rate and WLED sample rate are not synchronized
bool samplePeak = 0;                            // Boolean flag for peak. Responding routine must reset this flag
bool udpSamplePeak = 0;                         // Boolean flag for peak. Set at the same tiem as samplePeak, but reset by transmitAudioData
int delayMs = 1;                                // I don't want to sample too often and overload WLED
int micIn;                                      // Current sample starts with negative values and large values, which is why it's 16 bit signed
int sample;                                     // Current sample
int sampleAdj;                                  // Gain adjusted sample value
int sampleAgc;                                  // Our AGC sample
uint16_t micData;                               // Analog input for FFT
uint16_t micDataSm;                             // Smoothed mic data, as it's a bit twitchy
long timeOfPeak = 0;
long lastTime = 0;
float micLev = 0;                               // Used to convert returned value to have '0' as minimum. A leveller
float multAgc;                                  // sample * multAgc = sampleAgc. Our multiplier
float sampleAvg = 0;                            // Smoothed Average
double beat = 0;                                // beat Detection


struct audioSyncPacket {
  char header[6] = UDP_SYNC_HEADER;
  uint8_t myVals[32];     //  32 Bytes
  int sampleAgc;          //  04 Bytes
  int sample;             //  04 Bytes
  float sampleAvg;        //  04 Bytes
  bool samplePeak;        //  01 Bytes
  uint8_t fftResult[16];  //  16 Bytes
  double FFT_Magnitude;   //  08 Bytes
  double FFT_MajorPeak;   //  08 Bytes
};

bool isValidUdpSyncVersion(char header[6]) {
  return (header == UDP_SYNC_HEADER);
}

void getSample() {
  static long peakTime;
  extern double FFT_Magnitude;                    // Optional inclusion for our volume routines
  extern double FFT_MajorPeak;                    // Same here. Not currently used though

  #ifdef WLED_DISABLE_SOUND
    micIn = inoise8(millis(), millis());          // Simulated analog read
  #else
    micIn = micDataSm;      // micDataSm = ((micData * 3) + micData)/4;
//////
    DEBUGSR_PRINT("micIn:\tmicData:\tmicIn>>2:\tmic_In_abs:\tsample:\tsampleAdj:\tsampleAvg:\n");
    DEBUGSR_PRINT(micIn); DEBUGSR_PRINT("\t"); DEBUGSR_PRINT(micData);

    if (digitalMic == false) micIn = micIn >> 2;  // ESP32 has 2 more bits of A/D than ESP8266, so we need to normalize to 10 bit.
//////
    DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(micIn);
  #endif
  micLev = ((micLev * 31) + micIn) / 32;          // Smooth it out over the last 32 samples for automatic centering
  micIn -= micLev;                                // Let's center it to 0 now
  micIn = abs(micIn);                             // And get the absolute value of each sample
//////
  DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(micIn);
  //lastSample = micIn;

  // Using a ternary operator, the resultant sample is either 0 or it's a bit smoothed out with the last sample.
  sample = (micIn <= soundSquelch) ? 0 : (sample * 3 + micIn) / 4;
//////
  DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(sample);

  sampleAdj = sample * sampleGain / 40 + sample / 16; // Adjust the gain.
  sampleAdj = min(sampleAdj, 255);
  sample = sampleAdj;                                 // We'll now make our rebase our sample to be adjusted.
  sampleAvg = ((sampleAvg * 15) + sample) / 16;       // Smooth it out over the last 16 samples.
//////
  DEBUGSR_PRINT("\t"); DEBUGSR_PRINT(sample);
  DEBUGSR_PRINT("\t\t"); DEBUGSR_PRINT(sampleAvg); DEBUGSR_PRINT("\n\n");

  if (millis() - timeOfPeak > MIN_SHOW_DELAY) {       // Auto-reset of samplePeak after a complete frame has passed.
    samplePeak = 0;
    udpSamplePeak = 0;
    }

  if (userVar1 == 0) samplePeak = 0;
  // Poor man's beat detection by seeing if sample > Average + some value.
  if (sampleAgc > (sampleAvg + maxVol) && millis() > (peakTime + 100)) {
  // Then we got a peak, else we don't. Display routines need to reset the samplepeak value in case they miss the trigger.
    samplePeak = 1;
    timeOfPeak = millis();
    udpSamplePeak = 1;
    userVar1 = samplePeak;
    peakTime=millis();
  }
} // getSample()

/*
 * A simple averaging multiplier to automatically adjust sound sensitivity.
 */
void agcAvg() {

  multAgc = (sampleAvg < 1) ? targetAgc : targetAgc / sampleAvg;  // Make the multiplier so that sampleAvg * multiplier = setpoint
  sampleAgc = sample * multAgc;
  if (sampleAgc > 255) sampleAgc = 0;
  userVar0 = sampleAvg * 4;
  if (userVar0 > 255) userVar0 = 255;
} // agcAvg()



////////////////////
// Begin FFT Code //
////////////////////

#include "arduinoFFT.h"

void transmitAudioData() {
  if (!udpSyncConnected) return;
  extern uint8_t myVals[];
  extern int sampleAgc;
  extern int sample;
  extern float sampleAvg;
  extern bool udpSamplePeak;
  extern double fftResult[];
  extern double FFT_Magnitude;
  extern double FFT_MajorPeak;

  audioSyncPacket transmitData;

  for (int i = 0; i < 32; i++) {
    transmitData.myVals[i] = myVals[i];
  }

  transmitData.sampleAgc = sampleAgc;
  transmitData.sample = sample;
  transmitData.sampleAvg = sampleAvg;
  transmitData.samplePeak = udpSamplePeak;
  udpSamplePeak = 0;                              // Reset udpSamplePeak after we've transmitted it

  for (int i = 0; i < 16; i++) {
    transmitData.fftResult[i] = (uint8_t)constrain(fftResult[i], 0, 254);
  }

  transmitData.FFT_Magnitude = FFT_Magnitude;
  transmitData.FFT_MajorPeak = FFT_MajorPeak;

  fftUdp.beginMulticastPacket();
  fftUdp.write(reinterpret_cast<uint8_t *>(&transmitData), sizeof(transmitData));
  fftUdp.endPacket();
  return;
} // transmitAudioData()

const uint16_t samples = 512;                     // This value MUST ALWAYS be a power of 2
// The line below was replaced by  'const int SAMPLE_RATE = 10240'
//const double samplingFrequency = 10240;           // Sampling frequency in Hz
unsigned int sampling_period_us;
unsigned long microseconds;

double FFT_MajorPeak = 0;
double FFT_Magnitude = 0;
uint16_t mAvg = 0;

// These are the input and output vectors.  Input vectors receive computed results from FFT.
double vReal[samples];
double vImag[samples];
double fftBin[samples];
double fftBinMax[samples];

double fftResult[16];

// Table of linearNoise results to be multiplied by soundSquelch in order to reduce squelch across fftResult bins.
int linearNoise[16] = { 30, 28, 26, 25, 20, 12, 9, 6, 4, 4, 3, 2, 2, 2, 2, 2 };


// Try and normalize to a max value of 4096, so that 4096/16 = 256
float fftBinMult[256] = {0,0,0,
0.842,0.938,1.010,0.995,0.966,0.899,0.970,0.982,0.835,0.906,0.872,0.981,0.961,0.948,1.027,1.015,
0.820,0.859,0.643,0.656,0.695,0.739,0.647,0.572,0.463,0.413,0.455,0.494,0.440,0.366,0.457,0.364,
0.336,0.308,0.300,0.398,0.421,0.327,0.455,0.319,0.319,0.328,0.318,0.293,0.385,0.369,0.381,0.395,
0.475,0.342,0.363,0.357,0.366,0.360,0.399,0.380,0.446,0.417,0.390,0.413,0.280,0.345,0.406,0.498,
0.433,0.441,0.472,0.409,0.370,0.416,0.429,0.383,0.414,0.434,0.471,0.388,0.337,0.325,0.313,0.277,
0.265,0.305,0.275,0.316,0.284,0.266,0.337,0.290,0.330,0.337,0.417,0.398,0.362,0.306,0.332,0.315,
0.287,0.311,0.356,0.326,0.363,0.278,0.283,0.301,0.300,0.267,0.285,0.330,0.324,0.321,0.330,0.306,
0.282,0.239,0.234,0.235,0.209,0.237,0.222,0.211,0.210,0.169,0.181,0.206,0.177,0.156,0.153,0.156,
0.132,0.137,0.118,0.133,0.140,0.142,0.121,0.110,0.118,0.117,0.117,0.112,0.112,0.116,0.126,0.132,
0.137,0.137,0.146,0.147,0.155,0.144,0.149,0.145,0.165,0.139,0.143,0.150,0.150,0.138,0.143,0.159,
0.150,0.151,0.160,0.159,0.157,0.151,0.172,0.166,0.156,0.157,0.152,0.154,0.159,0.149,0.146,0.147,
0.170,0.136,0.161,0.136,0.152,0.140,0.147,0.172,0.143,0.157,0.162,0.161,0.165,0.200,0.240,0.193,
0.202,0.227,0.250,0.290,0.277,0.277,0.229,0.266,0.223,0.246,0.225,0.234,0.211,0.196,0.197,0.178,
0.185,0.198,0.185,0.195,0.156,0.163,0.182,0.166,0.163,0.129,0.157,0.142,0.147,0.160,0.146,0.123,
0.138,0.133,0.109,0.144,0.122,0.133,0.138,0.123,0.144,0.113,0.132,0.134,0.150,0.152,0.144,0.165,
0.170,0.166,0.171,0.139,0.172,0.156,0.150,0.145,0.157,0.154,0.161,0.157,0.181};

float avgChannel[16];    // This is a smoothed rolling average value for each bin. Experimental for AGC testing.



// Create FFT object
arduinoFFT FFT = arduinoFFT( vReal, vImag, samples, SAMPLE_RATE );

double fftAdd( int from, int to) {
  int i = from;
  double result = 0;
  while ( i <= to) {
    result += fftBin[i++];
  }
  return result;
}

// FFT main code
void FFTcode( void * parameter) {
  //DEBUG_PRINT("FFT running on core: "); DEBUG_PRINTLN(xPortGetCoreID());
  double beatSample = 0;
  double envelope = 0;

  for(;;) {
    delay(1);           // DO NOT DELETE THIS LINE! It is needed to give the IDLE(0) task enough time and to keep the watchdog happy.
                        // taskYIELD(), yield(), vTaskDelay() and esp_task_wdt_feed() didn't seem to work.

    microseconds = micros();
    extern double volume;

    for(int i=0; i<samples; i++) {
      if (digitalMic == false) {
        micData = analogRead(MIC_PIN);          // Analog Read
      } else {
        int32_t digitalSample = 0;
        int bytes_read = i2s_pop_sample(I2S_PORT, (char *)&digitalSample, portMAX_DELAY); // no timeout
        if (bytes_read > 0) {
          micData = abs(digitalSample >> 16);
        }
      }

      micDataSm = ((micData * 3) + micData)/4;  // We'll be passing smoothed micData to the volume routines as the A/D is a bit twitchy.
      vReal[i] = micData;                       // Store Mic Data in an array
      vImag[i] = 0;

      // MIC DATA DEBUGGING
      // DEBUGSR_PRINT("micData: ");
      // DEBUGSR_PRINT(micData);
      // DEBUGSR_PRINT("\tmicDataSm: ");
      // DEBUGSR_PRINT("\t");
      // DEBUGSR_PRINT(micDataSm);
      // DEBUGSR_PRINT("\n");

      while(micros() - microseconds < sampling_period_us){/*empty loop*/}

      microseconds += sampling_period_us;
    }

    FFT.Windowing( FFT_WIN_TYP_HAMMING, FFT_FORWARD );      // Weigh data
    FFT.Compute( FFT_FORWARD );                             // Compute FFT
    FFT.ComplexToMagnitude();                               // Compute magnitudes

    //
    // vReal[3 .. 255] contain useful data, each a 20Hz interval (60Hz - 5120Hz).
    // There could be interesting data at bins 0 to 2, but there are too many artifacts.
    //
    FFT.MajorPeak(&FFT_MajorPeak, &FFT_Magnitude);          // let the effects know which freq was most dominant

    for (int i = 0; i < samples; i++) {                     // Values for bins 0 and 1 are WAY too large. Might as well start at 3.
      double t = 0.0;
      t = abs(vReal[i]);
      t = t / 16.0;                                         // Reduce magnitude somewhat.
      fftBin[i] = t;

      fftBin[i] = fftBin[i]*fftBinMult[i];                  // Normalize the resultant values to 5000.

    }


    if (millis() > 100000 && millis() <100500) {

      for (int i=3; i<256; i++) {
        Serial.print(i); Serial.print("\t"); Serial.println(fftBinMax[i]);
      }

    }


/* Andrew's updated mapping of 256 bins down to the 16 result bins with Sample Freq = 10240, samples = 512.
 * Based on testing, the lowest/Start frequency is 60 Hz (with bin 3) and a highest/End frequency of 5120 Hz in bin 255.
 * Now, Take the 60Hz and multiply by 1.320367784 to get the next frequency and so on until the end. Then detetermine the bins.
 * End frequency = Start frequency * multiplier ^ 16
 * Multiplier = (End frequency/ Start frequency) ^ 1/16
 * Multiplier = 1.320367784
 */

//                                              Range      |  Freq | Max vol on MAX9814 @ 40db gain.
/*
      fftResult[0] = (fftAdd(3,4)) /2;        // 60 - 100    -> 82Hz,  26000
      fftResult[1] = (fftAdd(4,5)) /2;        // 80 - 120    -> 104Hz, 44000
      fftResult[2] = (fftAdd(5,7)) /3;        // 100 - 160   -> 130Hz, 66000
      fftResult[3] = (fftAdd(7,9)) /3;        // 140 - 200   -> 170,   72000
      fftResult[4] = (fftAdd(9,12)) /4;       // 180 - 260   -> 220,   60000
      fftResult[5] = (fftAdd(12,16)) /5;      // 240 - 340   -> 290,   48000
      fftResult[6] = (fftAdd(16,21)) /6;      // 320 - 440   -> 400,   41000
      fftResult[7] = (fftAdd(21,28)) /8;      // 420 - 600   -> 500,   30000
      fftResult[8] = (fftAdd(29,37)) /10;     // 580 - 760   -> 580,   25000
      fftResult[9] = (fftAdd(37,48)) /12;     // 740 - 980   -> 820,   22000
      fftResult[10] = (fftAdd(48,64)) /17;    // 960 - 1300  -> 1150,  16000
      fftResult[11] = (fftAdd(64,84)) /21;    // 1280 - 1700 -> 1400,  14000
      fftResult[12] = (fftAdd(84,111)) /28;   // 1680 - 2240 -> 1800,  10000
      fftResult[13] = (fftAdd(111,147)) /37;  // 2220 - 2960 -> 2500,  8000
      fftResult[14] = (fftAdd(147,194)) /48;  // 2940 - 3900 -> 3500,  7000
      fftResult[15] = (fftAdd(194, 255)) /62; // 3880 - 5120 -> 4500,  5000
*/

// Not_Matt version from LedFx                  Range      |  Freq | Max vol on MAX9814 @ 40db gain.
      fftResult[0] = (fftAdd(2,4)) /3;
      fftResult[1] = (fftAdd(4,6)) /3;
      fftResult[2] = (fftAdd(6,9)) /4;
      fftResult[3] = (fftAdd(9,13)) /5;
      fftResult[4] = (fftAdd(13,18)) /5;
      fftResult[5] = (fftAdd(18,24)) /6;
      fftResult[6] = (fftAdd(24,31)) /8;
      fftResult[7] = (fftAdd(31,40)) /10;
      fftResult[8] = (fftAdd(40,50)) /11;
      fftResult[9] = (fftAdd(50,63)) /14;
      fftResult[10] = (fftAdd(63,78)) /16;
      fftResult[11] = (fftAdd(78,97)) /20;
      fftResult[12] = (fftAdd(97,120)) /24;
      fftResult[13] = (fftAdd(120,148)) /29;
      fftResult[14] = (fftAdd(148,182)) /35;
      fftResult[15] = (fftAdd(182, 255)) /74;








  //  Linear noise supression of fftResult bins.
/*
    for (int i=0; i < 16; i++) {
        fftResult[i] = fftResult[i]-(float)soundSquelch*(float)linearNoise[i]/4.0 <= 0? 0 : fftResult[i]-(float)soundSquelch*(float)linearNoise[i]/4.0;
    }
*/

  // Print the fftResults
//    for (int i = 0; i< 16; i++) { Serial.print(fftResult[i]); Serial.print("\t"); }
//    Serial.println(" ");

  // Normalization of fftResult bins.


  } // for
} // FFTcode()



void logAudio() {
#ifdef MIC_LOGGER
  Serial.print(micIn);      Serial.print(" ");
  Serial.print(sample);     Serial.print(" ");
  Serial.print(sampleAvg);  Serial.print(" ");
  Serial.print(sampleAgc);  Serial.print(" ");
  Serial.print(micData);    Serial.print(" ");
  Serial.print(micDataSm);  Serial.print(" ");
#endif

#ifdef MIC_SAMPLING_LOG
  //------------ Oscilloscope output ---------------------------
  Serial.print(targetAgc); Serial.print(" ");
  Serial.print(multAgc); Serial.print(" ");
  Serial.print(sampleAgc); Serial.print(" ");

  Serial.print(sample); Serial.print(" ");
  Serial.print(sampleAvg); Serial.print(" ");
  Serial.print(micLev); Serial.print(" ");
  Serial.print(samplePeak); Serial.print(" ");    //samplePeak = 0;
  Serial.print(micIn); Serial.print(" ");
  Serial.print(100); Serial.print(" ");
  Serial.print(0); Serial.print(" ");
  Serial.println(" ");
#endif

#ifdef FFT_SAMPLING_LOG
  for(int i=0; i<16; i++) {
    Serial.print((int)constrain(fftResult[i],0,254));
    Serial.print(" ");
  }
  Serial.println("");
#endif
} // logAudio()
