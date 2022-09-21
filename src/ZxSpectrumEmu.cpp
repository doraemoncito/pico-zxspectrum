#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/pwm.h"


#include "vga.h"
#include "ZxSpectrumPrepareRgbScanline.h"
// #include "pzx_keyscan.h"

#include "PicoCharRendererVga.h"
#include "PicoWinHidKeyboard.h"
#include "PicoDisplay.h"
#include "PicoPen.h"
#include "PicoTextField.h"
#include "PicoWinHidKeyboard.h"

#include "ZxSpectrumFatSpiKiosk.h"
#include "ZxSpectrum.h"
#include "ZxSpectrumHidKeyboard.h"
#include "ZxSpectrumDualJoystick.h"
#include "ZxSpectrumHidJoystick.h"
// #include "ZxSpectrumPicomputerJoystick.h"

#include "bsp/board.h"
#include "tusb.h"
#include <pico/printf.h>
#include "SdCardFatFsSpi.h"
#include "QuickSave.h"
#include "ZxSpectrumFatFsSpiFileLoop.h"

#include "PicoWinHidKeyboard.h"
#include "PicoDisplay.h"
#include "ZxSpectrumMenu.h"
#include "ZxSpectrumAudio.h"
#include "ZxSpectrumEmuVga.h"

#define VREG_VSEL VREG_VOLTAGE_1_10

struct semaphore   ;
static const sVmode* vmode = NULL;

uint8_t* screenPtr;
uint8_t* attrPtr;

static bool showMenu = true;
static bool toggleMenu = false;
static volatile uint _frames = 0;

std::function<void()> _scanline_extra;
std::function<void()> _frame_extra;

void __not_in_flash_func(core1_main)() {
  sem_acquire_blocking(&core1_start_sme);
  printf("Core 1 running...\n");

  // TODO fetch the resolution from the mode ?
  VgaInit(vmode,640,480);

  while (1) {

    VgaLineBuf *linebuf = get_vga_line();
    uint32_t* buf = (uint32_t*)&(linebuf->line);
    uint32_t y = linebuf->row;
    if (showMenu) {
      pcw_prepare_vga332_scanline_80(
        buf,
        y,
        linebuf->frame);
    }
    else {
      zx_prepare_rgb_scanline(
        buf, 
        y, 
        linebuf->frame,
        screenPtr,
        attrPtr,
        zxSpectrum.borderColour()
      );
    }
    
    _scanline_extra();
//    pzx_keyscan_row();
    
    if (y == 239) { // TODO use a const / get from vmode
      
      // TODO Tidy this mechanism up
      screenPtr = zxSpectrum.screenPtr();
      attrPtr = screenPtr + (32 * 24 * 8);
      
      if (toggleMenu) {
        showMenu = !showMenu;
        toggleMenu = false;
        _frame_extra();
//        picomputerJoystick.enabled(!showMenu);
      }
      
      _frames = linebuf->frame;
    }    
  }
  __builtin_unreachable();
}

void __not_in_flash_func(main_loop)(std::function<void()> main_loop_extra){

  unsigned int lastInterruptFrame = _frames;

  //Main Loop 
  uint frames = 0;
  
  while(1){
    
    tuh_task();
    
    main_loop_extra();

//    hid_keyboard_report_t const *curr;
//    hid_keyboard_report_t const *prev;
//    pzx_keyscan_get_hid_reports(&curr, &prev);
//    process_picomputer_kbd_report(curr, prev);
    
    if (!showMenu) {
      for (int i = 1; i < 100; ++i) {
        if (lastInterruptFrame != _frames) {
          lastInterruptFrame = _frames;
          zxSpectrum.interrupt();
        }
        zxSpectrum.step();
      }
    }
    else if (frames != _frames) {
      frames = _frames;
      picoDisplay.refresh();
    }
  }
}

int main_vga(
  ZxSpectrum *pzxSpectrum,
  PicoDisplay *ppicoDisplay,
  std::function<void()> post_init,
  std::function<void()> main_loop_extra,
  std::function<void()> scanline_extra,
  std::function<void()> frame_extra,
){
  _scanline_extra = scanline_extra
  _frame_extra = frame_extra;
  
  vreg_set_voltage(VREG_VSEL);
  sleep_ms(10);
  vmode = Video(DEV_VGA, RES_HVGA);
  sleep_ms(100);

  // Initialise I/O
  stdio_init_all(); 
  
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  tusb_init();

  // Configure the GPIO pins for audio
  zxSpectrumAudioInit();

  screenPtr = zxSpectrum.screenPtr();
  attrPtr = screenPtr + (32 * 24 * 8);

//  keyboard1.setZxSpectrum(&zxSpectrum);
//  keyboard2.setZxSpectrum(&zxSpectrum);
  
  // Initialise the menu renderer
  pcw_init_renderer();

//  // Initialise the keyboard scan
//  pzx_keyscan_init();  
  // Initialise anything else
  post_init();
  
  sleep_ms(10);
  
  sem_init(&core1_start_sme, 0, 1);
  
  multicore_launch_core1(core1_main);

  picoRootWin.showMessage([=](PicoPen *pen) {
    pen->printAtF(3, 1, false, "Reading from SD card...");
  });
          
  picoDisplay.refresh();
  
  sem_release(&core1_start_sme);
 
  if (sdCard0.mount()) {
		
		// Set up the quick load loops
		zxSpectrumSnaps.reload();
		zxSpectrumTapes.reload();

    // Load quick save slot 1 if present
		if (quickSave.used(0)) {
			quickSave.load(&zxSpectrum, 0);
		}
  
    // See if the board is in kiosk mode    
    bool isKiosk = zxSpectrumKisok.isKiosk();
    keyboard1.setKiosk(isKiosk);
    keyboard2.setKiosk(isKiosk);
	}

  showMenu = false;
  picoRootWin.removeMessage();
}

class ZxSpectrumEmu {
  ZxSpectrum *_zxSpectrum,
  PicoDisplay *_picoDisplay,
  unsigned int _frames;
  
public:

  void __not_in_flash_func(main_loop)(
    ZxSpectrum *pzxSpectrum,
    PicoDisplay *ppicoDisplay,
    std::function<void()> *poll
  ){
    unsigned int lastInterruptFrame = _frames;

    //Main Loop 
    uint frames = 0, c = 0;
    
    while(1){

      poll[c++ & 1]();  
      
      if (!showMenu) {
        for (int i = 1; i < 100; ++i) {
          if (lastInterruptFrame != _frames) {
            lastInterruptFrame = _frames;
            pzxSpectrum->interrupt();
          }
          pzxSpectrum->step();
        }
      }
      else if (frames != _frames) {
        frames = _frames;
        ppicoDisplay->refresh();
      }
    }
    
    __builtin_unreachable(); 
  } 
};


void __not_in_flash_func(main_loop)(
  ZxSpectrum *pzxSpectrum,
  PicoDisplay *ppicoDisplay,
  std::function<void()> *poll
){
  unsigned int lastInterruptFrame = _frames;

  //Main Loop 
  uint frames = 0, c = 0;
  
  while(1){

    poll[c++ & 1]();  
    
    if (!showMenu) {
      for (int i = 1; i < 100; ++i) {
        if (lastInterruptFrame != _frames) {
          lastInterruptFrame = _frames;
          pzxSpectrum->interrupt();
        }
        pzxSpectrum->step();
      }
    }
    else if (frames != _frames) {
      frames = _frames;
      ppicoDisplay->refresh();
    }
  }
  
  __builtin_unreachable(); 
}

