
#include <SPI.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <vs1063.h>
#include <SdFat.h>
#include <IRremote.h>
#include "playa.h"

#define PIN_IR          2       // digital pin for the IR remote
#define PIN_CARD_CS     10      // digital pin for uSD card chip select

// analog pins
#define PIN_JACK_DETECT A1      // detect if stereo jack is not connected
#define PIN_VBAT_DETECT A5      // battery voltage readout
#define PIN_RANDOM      A6      // seed the pseudo RNG by reading this unconnected pin

#define CARD_BUFF_SZ    512     // how much data to read from the uSD card in one go
#define VS_BUFF_SZ      32      // how much data to send in one batch to VS1063
#define MAX_PATH        40      // enough for 4 parent dirs for one file

#define CMD_NULL        0x00
#define CMD_EXIT        0x01
#define CMD_PAUSE       0x02

#define STOP            0x10
#define PLAY_RANDOM     0x11
#define SWITCH_TO_ALBUM 0x12
#define PLAY_ALBUM      0x13

// vs1063
uint8_t cbuff[CARD_BUFF_SZ];
uint8_t volume = 40;            // as negative attenuation. can go from 0x00 lound - 0xfe silent
                     // 0xff is a special case (analog powerdown mode)

// sdfat
SdFat sd;
SdFile file;
char fs_entity[13];
uint8_t path_level;
char file_path[MAX_PATH];
char album_path[MAX_PATH];

// infrared remote
IRrecv irrecv(PIN_IR);
decode_results results;
uint32_t result_last = 11111;
uint16_t ir_delay = 2000;   // delay between repeated button presses
uint32_t ir_delay_prev = 0;
uint8_t ir_cmd;
uint8_t play_mode_prev;
uint8_t play_mode = PLAY_RANDOM;
uint8_t mute = false;
uint16_t in_number = 0;

// misc
uint16_t seed;

// sleep states
volatile uint8_t sleeping = 0, just_woken = 0;
uint32_t wake_up_time = 0;      // systemtime when the uC was woken up
uint16_t wake_up_delay = 2000;  // the delay until the uC is powered down again

ISR(INT0_vect)
{
    EIFR |= 0x1;                // reset INTF0
    if (sleeping) {
        just_woken = 1;
        sleeping = 0;
    }
}

void setup()
{

    delay(1000);                // switch/jumper/ammeter debounce

#ifdef DEBUG
    Serial.begin(9600);
#else
    power_usart0_disable();
#endif
    power_twi_disable();

    wdt_enable(WDTO_8S);        // Enable watchdog: max 8 seconds

    SPI.begin();
    SPI.setBitOrder(MSBFIRST);
    SPI.setDataMode(SPI_MODE0);

    vs_setup();
    vs_setup_local();

    irrecv.enableIRIn();

    if (!sd.init(SPI_FULL_SPEED, PIN_CARD_CS))
        sd.initErrorHalt();

}

void loop()
{
    uint16_t vbat, jack_detect;
    uint32_t now;
    uint8_t i;
    ir_decode();
    wdt_reset();

    now = millis();

    if (play_mode != STOP) {

        // jack_detect is zero if a stereo jack is physically plugged in. 
        // otherwise the common voltage for the earphones (aka GBUF) is read (1.65v)
        //
        // vbat's value is (1024/Vref)*Vbattery*R2/(R1+R2), where
        //    Vref     - ADC voltage reference - 3.3v
        //    Vbattery - actual battery voltage (should be between 3 and 4.2v)
        //    R2       - 68Kohm
        //    R1       - 39Kohm
        // we try to limit current consumption when the Lipo cell reaches about 3.6v.
        // Note:
        //   this formula is usable only when Vref is actually 3.3v. 
        //   in case the cell voltage is below ~3.5v, vbat is not read correctly
        //   and the only thing protecting the cell is the internal cutoff

        vbat = analogRead(PIN_VBAT_DETECT);
        jack_detect = analogRead(PIN_JACK_DETECT);

        // attempt to gather some entropy from the outside
        // sometimes PIN_RANDOM gives the same values, so keep and increment seed between runs
        seed += millis();
        for (i = 0; i < vbat / 10; i++) {
            seed += analogRead(PIN_RANDOM);
        }
        randomSeed(seed);
        for (i = 0; i < vbat / 10; i++) {
            random();           // apparently randomSeed does not provide proper randomness
        }

        if ((vbat < 712) || (jack_detect > 0)) {
            // shut down vs1063 to protect the Lipo cell
            vs_assert_xreset();
            play_mode_prev = play_mode;
            play_mode = STOP;
        }
    } else if (just_woken == 0) {
        pwr_down();
    } else if ((just_woken == 1) && (wake_up_time == 0)) {
        wake_up_time = now;
    } else if ((just_woken == 1) && (now - wake_up_time > wake_up_delay)) {
        // if an IR signal woke up the uC from sleep, but that signal was not 
        // a power sequence then go back to sleep.
        wake_up_time = 0;
        pwr_down();
    }

    switch (play_mode) {
    case STOP:
        break;
    case PLAY_RANDOM:
        path_level = 0;
        sd.chdir("/");
        memset(file_path, 0, MAX_PATH);
        file_find_random();
        break;
    case PLAY_ALBUM:
        file_find_next();
        break;
    case SWITCH_TO_ALBUM:
        play_mode = PLAY_ALBUM;
        get_album_path();
        sd.chdir(album_path);
        sd.vwd()->rewind();
        file_find_next();
        break;
    }

}

void vs_setup_local()
{
    //initialize chip 
    vs_deselect_control();
    vs_deselect_data();
    vs_set_volume(0xff, 0xff);
    //AVDD is at least 3.3v, so select 1.65v reference to increase 
    //the analog output swing
    vs_write_register(SCI_STATUS, SS_REFERENCE_SEL);
    // Declick: Slow sample rate for slow analog part startup
    vs_write_register(SCI_AUDATA, 0, 10);       // 10 Hz
    //delay(100);
    // Switch on the analog parts
    vs_write_register(SCI_AUDATA, 31, 64);      // 8kHz
    vs_soft_reset();
    vs_set_volume(volume, volume);
}

void ir_decode()
{
    unsigned long now;
    int8_t ir_number = -1;

    now = millis();

    if (irrecv.decode(&results)) {

        if ((results.decode_type == RC5) && (results.value >= 2048))
            results.value -= 2048;

        // if we woke up from sleep, only allow a power-up command
        if ((just_woken) && (results.value != 12)) {
            results.value = 11111;
        }

        if ((results.value == result_last) && (now - ir_delay_prev < ir_delay)) {
            results.value = 11111;
        }

        switch (results.value) {
            // RC5 codes
        case 1:                // 1
            ir_number = 1;
            break;
        case 2:                // 2
            ir_number = 2;
            break;
        case 3:                // 3
            ir_number = 3;
            break;
        case 4:                // 4
            ir_number = 4;
            break;
        case 5:                // 5
            ir_number = 5;
            break;
        case 6:                // 6
            ir_number = 6;
            break;
        case 7:                // 7
            ir_number = 7;
            break;
        case 8:                // 8
            ir_number = 8;
            break;
        case 9:                // 9
            ir_number = 9;
            break;
        case 0:                // 0
            ir_number = 0;
            break;
//        case 10: // 10
//            ir_number = 10;
//            break;
        case 12: // power
            // wake up from pwr_down
            if (just_woken == 1) {
                just_woken = 0;
                wake_up_time = 0;
                //play_mode = play_mode_prev;
                play_mode = PLAY_RANDOM;
                power_spi_enable();
                vs_deassert_xreset();
                vs_setup();
                vs_setup_local();
            }
            break;
        case 56:               // AV
            in_number = 0;
            break;
/*        case 36: // red
          break;
        case 35: // green
          break;
        case 14: // yellow
          break;
        case 50: // zoom
            break;
        case 39: // sub
            break;
        case 44: // slow
            break;
        case 60: // repeat
            break;
        case 15: // disp
            break;
        case 38: // sleep
            break;
        case 32: // up
            break;
        case 33: // down
            break;
        case 16: // right
            break;
        case 17: // left
            break;
        case 59: // ok
            break;
        case 34: // back
            break;
        case 19: // exit
            break;
        case 18: // menu
            break;
*/
        case 13:
        case 0x290:            // mute
            mute = true;
            vs_set_volume(0xfe, 0xfe);
            break;
        case 16:
        case 0x490:            // vol+
            mute = false;
            if (volume > 3) {
                volume -= 4;    // decrease attenuation by 2dB
                vs_set_volume(volume, volume);
            }
            break;
        case 17:
        case 0xc90:            // vol-
            mute = false;
            if (volume < 251) {
                volume += 4;    // increase attenuation by 2dB
                vs_set_volume(volume, volume);
            }
            break;
        case 28:
        case 0x90:             // ch+
            ir_cmd = CMD_EXIT;
            vs_write_register(SCI_MODE, SM_CANCEL);
            break;
        case 29:
        case 0x890:            // ch-
            in_number++;
            ir_cmd = CMD_EXIT;
            vs_write_register(SCI_MODE, SM_CANCEL);
            break;
//        case 36:               // record
//            break;
        case 54:
        case 0xa90:            // stop
            vs_write_register(SCI_MODE, SM_CANCEL);
            // to minimize the power-off transient
            vs_set_volume(0xfe, 0xfe);
            delay(10);
            vs_assert_xreset();
            play_mode_prev = play_mode;
            play_mode = STOP;
            ir_cmd = CMD_EXIT;
            break;
        case 14:               // play
            /*
            if (play_mode == STOP) {
                mute = false;
                vs_deassert_xreset();
                vs_wait();
                vs_setup_local();
                vs_set_volume(volume, volume);
            }
            */
            play_mode = PLAY_RANDOM;
            break;
//        case 31:               // pause
//            break;
        case 35:
        case 0xa50:            // rew, AV/TV
            if (in_number != 0) {
                in_number = 0;
            } else {
                if (play_mode == PLAY_RANDOM)
                    play_mode = SWITCH_TO_ALBUM;
                if (play_mode == PLAY_ALBUM)
                    play_mode = PLAY_RANDOM;
            }
            ir_cmd = CMD_EXIT;
            break;
//        case : // fwd
//            break;
//        default:
//            Serial.println(results.value);
//            break;
        }                       // case

        // vol+ and vol- should not care about ir_delay
        if ((results.value != 11111) && (results.value != 16)
            && (results.value != 17) && (results.value != 0xc90) &&
            (results.value != 0x490)) {
            result_last = results.value;
            ir_delay_prev = now;
        }
        // get a number from the ir remote
        if (ir_number > -1) {
            in_number = in_number * 10 + ir_number;
        }

        irrecv.resume();        // Receive the next keypress
    }

}

void get_album_path()
{
    uint8_t i;
    uint8_t l = 0;
    for (i = 0; i < MAX_PATH; i++) {
        if (file_path[i] == '/')
            l = i;
    }
    strncpy(album_path, file_path, l);
    album_path[l] = 0;          // album_path[] and file_pathp[] have the same size
    // so there is no buffer overflow here
}

uint8_t play_file()
{
    uint16_t i, r, vs_buff_end;
    uint8_t count = 0;
    uint8_t checked = false;
    uint16_t codec = 0x0eaa;    // something unused
    int16_t replaygain_offset = 0;
    uint8_t replaygain_volume;

    vs_soft_reset();

    //Serial.println(file_path);

    if (!file.open(file_path, O_READ)) {
        sd.errorHalt("open failed");
    }

    while ((r = file.read(cbuff, CARD_BUFF_SZ))) {
        if (!checked)
            count++;
        if (!checked && count > 10) {
            vs_deselect_data();
            // sometimes the decoder never gets busy while reading non-music data
            // so we exit here
            if (vs_read_register(SCI_HDAT1) == 0) {
                file.close();
                vs_write_register(SCI_MODE, SM_CANCEL);
                return 1;
            }
        }
        vs_select_data();
        i = 0;
        while (i < r) {
            while (!digitalRead(VS_DREQ)) {
                // the VS chip is busy, so do something else
                vs_deselect_data();     // Release the SDI bus
                wdt_reset();
                ir_decode();
                if (ir_cmd == CMD_EXIT || codec == 0) {
                    file.close();
                    ir_cmd = CMD_NULL;
                    vs_write_register(SCI_MODE, SM_CANCEL);
                    return 0;
                }
                if (!checked && count > 1) {
                    vs_deselect_data();
                    // do a one-time check of the codec status
                    codec = vs_read_register(SCI_HDAT1);
                    if (codec == 0x4f67) {
                        // if ogg, read the replaygain offset
                        replaygain_offset = vs_read_wramaddr(ogg_gain_offset);
                        if (replaygain_offset < 10 && replaygain_offset > -30) {
                            replaygain_volume =
                                volume - (replaygain_offset + 12);
                            if (!mute) {
                                vs_set_volume(replaygain_volume,
                                              replaygain_volume);
                            }
                            //Serial.println(volume);
                            //Serial.println(replaygain_offset);
                            //Serial.println(replaygain_volume);
                        }
                    }
                    checked = true;
                }
                vs_select_data();       // Pull XDCS low
            }                   // the mint rubbing function

            vs_buff_end = i + VS_BUFF_SZ - 1;
            if (vs_buff_end > r - 1) {
                vs_buff_end = r - 1;
            }
            // send up to 32bytes after a VS_DREQ check
            while (i <= vs_buff_end) {
                SPI.transfer(cbuff[i]); // Send SPI byte
                i++;
            }
        }
        vs_wait();
        vs_deselect_data();
    }

    vs_write_register(SCI_MODE, SM_CANCEL);
    //SendZerosToVS10xx();

    file.close();
    return 0;
}

uint8_t file_find_next()
{
    if (file.openNext(sd.vwd(), O_READ)) {
        file.getFilename(fs_entity);
        file.close();
        snprintf(file_path, MAX_PATH, "%s/%s", album_path, fs_entity);
    } else {
        // the album has ended
        play_mode = PLAY_RANDOM;
        return 0;
    }

    if (!sd.chdir(fs_entity)) {
        play_file();
        return 0;
    } else {
        return 1;
    }

    return 0;
}

uint8_t file_find_random()
{
    uint16_t i = 0, items = 0, rnd;

    // how many items in the current dir?
    while (file.openNext(sd.vwd(), O_READ)) {
        items++;
        file.close();
    }

    if (items == 0)
        return 1;

    if (path_level == 0) {
        if (in_number == 0) {
            // pick one at random, then either play it if it's a file
            // or cd into it and repeat until a file is found
            rnd = random(1, items + 1);
        } else {
            rnd = (in_number % items) + 1;
        }
    } else {
        rnd = random(1, items + 1);
    }

    sd.vwd()->rewind();

    for (i = 1; i <= rnd; i++) {
        file.openNext(sd.vwd(), O_READ);
        file.getFilename(fs_entity);
        file.close();
    }

    strncat(file_path, "/", 2);
    strncat(file_path, fs_entity, 14);

#ifdef DEBUG
    Serial.print("path_level ");
    Serial.print(path_level);
    Serial.print(", num_dirs ");
    Serial.print(items);
    Serial.print(", seed ");
    Serial.print(seed);
    Serial.print(", rnd ");
    Serial.print(rnd);
    Serial.print(", in_num ");
    Serial.print(in_number);
    Serial.print(", path ");
    Serial.println(file_path);
#endif

    if (!sd.chdir(fs_entity)) {
        play_file();
        return 0;
    } else {
        path_level++;
        if (path_level < 5)
            file_find_random();
    }
    return 0;
}

// used when STOP command is issued
void pwr_down()
{

    vs_assert_xreset();
    wdt_disable();
    power_spi_disable();

    sleeping = 1;

    // wake up on remote control input (external INT0 interrupt)
    EICRA = 0;                  //Interrupt on low level
    EIMSK = (1 << INT0);        // enable INT0 interrupt

    do {
        set_sleep_mode(SLEEP_MODE_PWR_DOWN);
        cli();
        sleep_enable();
        sei();
        sleep_cpu();
        sleep_disable();
        sei();
        EIMSK = 0;              // disable INT0/1 interrupts
    } while (0);

    // wake up
    wdt_enable(WDTO_8S);

}
