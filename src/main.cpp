/*
   Copyright (c) 2018, Adrian Rossiter

   Antiprism - http://www.antiprism.com

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

      The above copyright notice and this permission notice shall be included
      in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.
*/

#include "display.h"
#include "display_info.h"
#include "player.h"
#include "programopts.h"
#include "timer.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <math.h>
#include <string>
#include <vector>

using std::string;
using std::vector;

const int SPECT_WIDTH = 128;

ArduiPi_OLED display; // global, for use during signal handling

void cleanup(void)
{
  // Clear and close display
  display.invertDisplay(false);
  display.clearDisplay();
  display.display();
  display.close();
}

void signal_handler(int sig)
{
  switch (sig) {
  case SIGINT:
  case SIGHUP:
  case SIGTERM:
    cleanup();
    break;
  }
  abort();
}

void init_signals(void)
{
  struct sigaction new_action;
  memset(&new_action, 0, sizeof(new_action));
  new_action.sa_handler = &signal_handler;
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = 0;

  struct sigaction old_action;
  sigaction(SIGINT, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGINT, &new_action, NULL);
  sigaction(SIGHUP, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGHUP, &new_action, NULL);
  sigaction(SIGTERM, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGTERM, &new_action, NULL);
}

class OledOpts : public ProgramOpts {
public:
  const double DEF_SCROLL_RATE = 8;    // pixels per second
  const double DEF_SCROLL_DELAY = 5;   // second delay before scrolling
  int oled = OLED_ADAFRUIT_SPI_128x32; // OLED type, as a number
  int framerate = 15;                  // frame rate in Hz
  int bars = 16;                       // number of bars in spectrum
  int gap = 1;                         // gap between bars, in pixels
  vector<double> scroll;   // rate (pixels per sec), start delay (secs)
  int clock_format = 0;    // 0-3: 0,1 - 24h  2,3 - 12h  0,2 - leading 0
  int date_format = 0;     // 0: DD-MM-YYYY, 1: MM-DD-YYYY
  char pause_screen = 'p'; // p - play, s - stop
  string cava_prog_name = "mpd_oled_cava"; // cava executable name
  string cava_method = "fifo";             // fifo, alsa or pulse
  string cava_source;                      // Path to FIFO / alsa device
  double invert = 0;             // 0 normal, -1 invert, n>0 invert every n hrs
  bool alt_layout = false;       // -alt clock / fullscreen spectrum
  int saver = 0;                 // 0 off, 1 spin, 2 sine, 3 stripes, 4 noise
  double saver_speed = -1;      // cycles per minute, <0 uses the default
  int stripe_thickness = 1;     // pixels, stripes saver only
  int stripe_axis = 0;          // 0 xy, 1 x, 2 y
  int stripe_dir = 0;           // 0 either, 1 up, 2 down, 3 left, 4 right
  int noise_percent = 4;        // percent of pixels inverted
  int noise_dir = 2;            // 1 up, 2 down, 3 left, 4 right
  bool rotate180 = false;        // display upside down
  unsigned char i2c_addr = 0;    // number of I2C address
  int i2c_bus = 1;               // number of I2C bus
  int reset_gpio = 25;           // reset pin
  int spi_dc_gpio = OLED_SPI_DC; // SPI DC
  int spi_cs = OLED_SPI_CS0;     // SPI CS - 0: CS0, 1: CS1
  Player player;

  OledOpts() : ProgramOpts("mpd_oled", "0.02")
  {
    cava_source = "/tmp/" + get_program_name() + "_fifo";
    scroll.push_back(DEF_SCROLL_RATE);
    scroll.push_back(DEF_SCROLL_DELAY);
    scroll.push_back(DEF_SCROLL_RATE);
    scroll.push_back(DEF_SCROLL_DELAY);

    player.init_detect();
  }
  void process_command_line(int argc, char **argv);
  void usage();
};

void OledOpts::usage()
{
  fprintf(stdout, R"(
Usage: %s -o oled_type [options] [input_file]

Display information about an MPD-based player on an OLED screen

Options
%s)",
          get_program_name().c_str(), help_ver_text);

  fprintf(
      stdout,
      "  -o <type>  OLED type, specified as a number, from the following:\n");
  for (int i = 0; i < OLED_LAST_OLED; i++)
    if (strstr(oled_type_str[i], "128x64"))
      fprintf(stdout, "      %1d %s\n", i, oled_type_str[i]);

  fprintf(stdout,
          R"(  -b <num>   number of bars to display (default: 16)
  -g <sz>    gap between bars in, pixels (default: 1)
  -f <hz>    framerate in Hz (default: 15)
  -s <vals>  scroll rate (pixels per second) and start delay (seconds), up
             to four comma separated decimal values (default: %.1f,%.1f) as:
                rate_all
                rate_all,delay_all
                rate_title,delay_all,rate_artist
                rate_title,delay_title,rate_artist,delay_artist
  -C <fmt>   clock format: 0 - 24h leading 0 (default), 1 - 24h no leading 0,
                2 - 24h leading 0, 3 - 24h no leading 0
  -d         use USA date format MM-DD-YYYY (default: DD-MM-YYYY)
  -P <val>   pause screen type: p - play (default), s - stop
  -k         cava executable name is cava (default: mpd_oled_cava)
  -c         cava input method and source (default: '%s,%s')
             e.g. 'fifo,/tmp/my_fifo', 'alsa,hw:5,0', 'pulse'
  -R         rotate display 180 degrees
  -I <val>   invert black/white: n - normal (default), i - invert,
             number - switch between n and i with this period (hours), which
             may help avoid screen burn
  -alt       alternate layout. Stopped: time only. Playing: full-screen
             spectrum. Replaces -I
  -saver <k> screensaver. Optional speed after a comma. Replaces -I
             spin[,turns]  split through the center, clockwise (default 0.5
                           turns per minute)
             sine[,widths] sine split, one cycle across the screen, moving
                           right. Inverted on the wave side (default 1
                           screen per minute)
             stripes[,speed[,px[,axis[,dir]]]]
                           a new stripe every 1s. White paints the
                           inversion, black erases it. Speed is lengths per
                           minute (default 60). px is thickness (default 1).
                           axis is x, y, or xy (default xy). x direction is
                           up, down, or up-down. y direction is left, right,
                           or left-right. Default is either way
             noise[,px/s[,pct[,dir]]] sparse specks that travel.
                           Default 30 pixels per second, 4 percent, down.
                           dir is up, down, left, or right
  -a <addr>  I2C address, in hex (default: default for OLED type)
  -B num     I2C bus number (default: 1, giving device /dev/i2c-1)
  -r <gpio>  I2C/SPI reset GPIO number, if needed (default: 25)
  -D <gpio>  SPI DC GPIO number (default: 24)
  -S <num>   SPI CS number (default: 0)
  -p <plyr>  Player: mpd, moode, volumio, runeaudio (default: detected)
Example :
%s -o 6 use a %s OLED
)",
          DEF_SCROLL_RATE, DEF_SCROLL_DELAY, cava_method.c_str(),
          cava_source.c_str(), get_program_name().c_str(), oled_type_str[6]);
}

void OledOpts::process_command_line(int argc, char **argv)
{
  opterr = 0;
  int c;
  int method_len;

  for (int i = 1; i < argc;) {
    if (strcmp(argv[i], "-alt") != 0 && strcmp(argv[i], "--alt") != 0) {
      i++;
      continue;
    }
    alt_layout = true;
    for (int j = i; j < argc - 1; j++)
      argv[j] = argv[j + 1];
    argc--;
    argv[argc] = NULL;
  }

  for (int i = 1; i < argc;) {
    if (strcmp(argv[i], "-saver") != 0 && strcmp(argv[i], "--saver") != 0) {
      i++;
      continue;
    }
    if (i + 1 >= argc || argv[i + 1][0] == '-')
      error("-saver needs spin, sine, stripes, or noise", "-saver");
    string spec = argv[i + 1];
    string kind = spec;
    string speed_str;
    string thick_str;
    string axis_str;
    string dir_str;
    size_t comma = spec.find(',');
    if (comma != string::npos) {
      kind = spec.substr(0, comma);
      speed_str = spec.substr(comma + 1);
    }
    if (kind == "spin")
      saver = 1;
    else if (kind == "sine")
      saver = 2;
    else if (kind == "stripes")
      saver = 3;
    else if (kind == "noise")
      saver = 4;
    else
      error("saver must be spin, sine, stripes, or noise", "-saver");
    size_t comma2 = speed_str.find(',');
    if (comma2 != string::npos) {
      thick_str = speed_str.substr(comma2 + 1);
      speed_str = speed_str.substr(0, comma2);
    }
    size_t comma3 = thick_str.find(',');
    if (comma3 != string::npos) {
      axis_str = thick_str.substr(comma3 + 1);
      thick_str = thick_str.substr(0, comma3);
    }
    size_t comma4 = axis_str.find(',');
    if (comma4 != string::npos) {
      dir_str = axis_str.substr(comma4 + 1);
      axis_str = axis_str.substr(0, comma4);
    }
    if (kind == "noise" && !dir_str.empty())
      error("noise direction is up, down, left, or right", "-saver");
    if (kind == "noise" && !axis_str.empty()) {
      if (axis_str == "up")
        noise_dir = 1;
      else if (axis_str == "down")
        noise_dir = 2;
      else if (axis_str == "left")
        noise_dir = 3;
      else if (axis_str == "right")
        noise_dir = 4;
      else
        error("noise direction is up, down, left, or right", "-saver");
    }
    if (kind != "stripes" && kind != "noise" &&
        (!thick_str.empty() || !axis_str.empty() || !dir_str.empty()))
      error("only stripes takes a thickness, axis, and direction", "-saver");
    if (!speed_str.empty()) {
      if (!read_double(speed_str.c_str(), &saver_speed) || saver_speed < 0)
        error("saver speed must be a non-negative number", "-saver");
    }
    if (kind == "noise" && !thick_str.empty()) {
      if (!read_int(thick_str.c_str(), &noise_percent) || noise_percent < 0 ||
          noise_percent > 100)
        error("noise percent must be from 0 to 100", "-saver");
    }
    if (kind == "stripes" && !thick_str.empty()) {
      if (!read_int(thick_str.c_str(), &stripe_thickness) ||
          stripe_thickness < 1)
        error("stripe thickness must be a positive number of pixels",
              "-saver");
    }
    if (kind == "stripes" && !axis_str.empty()) {
      if (axis_str == "x")
        stripe_axis = 1;
      else if (axis_str == "y")
        stripe_axis = 2;
      else if (axis_str == "xy")
        stripe_axis = 0;
      else
        error("stripe axis must be x, y, or xy", "-saver");
    }
    if (kind == "stripes" && !dir_str.empty() && axis_str.empty())
      error("stripe direction needs an axis", "-saver");
    if (kind == "stripes" && !dir_str.empty()) {
      bool x_axis = stripe_axis != 2;
      bool y_axis = stripe_axis != 1;
      if (dir_str == "up" || dir_str == "down" || dir_str == "up-down") {
        if (!x_axis)
          error("y direction is left, right, or left-right", "-saver");
      }
      else if (dir_str == "left" || dir_str == "right" ||
               dir_str == "left-right") {
        if (!y_axis)
          error("x direction is up, down, or up-down", "-saver");
      }
      else
        error("stripe direction is up, down, up-down, left, right, or "
              "left-right",
              "-saver");
      if (dir_str == "up")
        stripe_dir = 1;
      else if (dir_str == "down")
        stripe_dir = 2;
      else if (dir_str == "left")
        stripe_dir = 3;
      else if (dir_str == "right")
        stripe_dir = 4;
      else
        stripe_dir = 0;
    }
    for (int j = i; j < argc - 2; j++)
      argv[j] = argv[j + 2];
    argc -= 2;
    argv[argc] = NULL;
  }

  handle_long_opts(argc, argv);

  while ((c = getopt(argc, argv, ":ho:b:g:f:s:C:dP:kc:RI:a:B:r:D:S:p:")) !=
         -1) {
    if (common_opts(c, optopt))
      continue;

    switch (c) {
    case 'o':
      print_status_or_exit(read_int(optarg, &oled), c);
      if (oled < 0 || oled >= OLED_LAST_OLED ||
          !strstr(oled_type_str[oled], "128x64"))
        error(msg_str("invalid 128x64 oled type %d (see -h)", oled), c);
      break;

    case 'b':
      print_status_or_exit(read_int(optarg, &bars), c);
      if (bars < 2 || bars > 60)
        error("select between 2 and 60 bars", c);
      break;

    case 'g':
      print_status_or_exit(read_int(optarg, &gap), c);
      if (gap < 0 || gap > 30)
        error("gap must be between 0 and 30 pixels", c);
      break;

    case 'f':
      print_status_or_exit(read_int(optarg, &framerate), c);
      if (framerate < 1)
        error("framerate must be a positive integer", c);
      break;

    case 's':
      print_status_or_exit(read_double_list(optarg, scroll, 4), c);
      if (scroll.size() < 1)
        scroll.push_back(DEF_SCROLL_RATE);
      else if (scroll[0] < 0)
        error("scroll rate cannot be negative", c);

      if (scroll.size() < 2)
        scroll.push_back(DEF_SCROLL_DELAY);
      else if (scroll[1] < 0)
        error("scroll delay cannot be negative", c);

      if (scroll.size() < 3)
        scroll.push_back(scroll[0]);
      else if (scroll[2] < 0)
        error("scroll rate (origin/artist) cannot be negative", c);

      if (scroll.size() < 4)
        scroll.push_back(scroll[1]);
      else if (scroll[3] < 0)
        error("scroll delay (origin/artist) cannot be negative", c);

      break;

    case 'C':
      print_status_or_exit(read_int(optarg, &clock_format), c);
      if (clock_format < 0 || clock_format > 3)
        error("clock format number is not 0, 1, 2 or 3", c);
      break;

    case 'd':
      date_format = 1;
      break;

    case 'P':
      if (strcmp(optarg, "p") == 0)
        pause_screen = 'p';
      else if (strcmp(optarg, "s") == 0)
        pause_screen = 's';
      else
        error("pause screen type is not p or s", c);
      break;

    case 'k':
      cava_prog_name = "cava";
      break;

    case 'c':
      method_len = 5; // all the initial method strings are length 5!
      if (strncmp(optarg, "fifo,", method_len) == 0) {
        cava_method = "fifo";
        if (optarg[method_len] == '\0')
          error("cava input method is fifo, but no FIFO path was specified", c);
      }
      else if (strncmp(optarg, "alsa,", method_len) == 0) {
        cava_method = "alsa";
        if (optarg[method_len] == '\0')
          error("cava input method is alsa, but no ALSA stream was specified",
                c);
      }
      else if (strncmp(optarg, "pulse", method_len) == 0) {
        cava_method = "pulse";
        if (optarg[method_len] != '\0')
          error("cava input method is pulse, but is followed by extra text", c);
      }
      else
        error("cava input specifier is not in form 'fifo,fifo_path', "
              "'alsa,alsa_stream', or 'pulse'",
              c);

      cava_source = &optarg[method_len];
      break;

    case 'R':
      rotate180 = true;
      break;

    case 'I':
      if (strcmp(optarg, "n") == 0)
        invert = 0;
      else if (strcmp(optarg, "i") == 0)
        invert = -1;
      else if (read_double(optarg, &invert)) {
        if (invert <= 0)
          error("number of hours for period must be positive number", c);
      }
      else
        error("invalid value, should be n, i or a positive number", c);
      break;

    case 'a':
      if (strlen(optarg) != 2 || strspn(optarg, "01234567890aAbBcCdDeEfF") != 2)
        error("I2C address should be two hexadecimal digits", c);

      i2c_addr = (unsigned char)strtol(optarg, NULL, 16);
      break;

    case 'B':
      print_status_or_exit(read_int(optarg, &i2c_bus), c);
      if (i2c_bus < 0)
        error("bus number cannot be negative", c);
      break;

    case 'r':
      print_status_or_exit(read_int(optarg, &reset_gpio), c);
      if (!isdigit(optarg[0]) || reset_gpio < 0 || reset_gpio > 99)
        error("probably invalid (not integer in range 0 - 99), specify the\n"
              "GPIO number of the pin that RST is connected to",
              c);
      break;

    case 'D':
      print_status_or_exit(read_int(optarg, &spi_dc_gpio), c);
      if (!isdigit(optarg[0]) || reset_gpio < 0 || reset_gpio > 99)
        error("probably invalid (not integer in range 0 - 99), specify the\n"
              "GPIO number of the pin that SPI DC is connected to",
              c);
      break;

    case 'S':
      print_status_or_exit(read_int(optarg, &spi_cs), c);
      if (spi_cs < 0 || spi_cs > 1)
        error("SPI CS should be 0 or 1", c);
      break;

    case 'p': {
      // const char *params = "mpd|moode|volumio|runeaudio\n";
      string params = Player::all_names("|");
      string arg_id;
      if (!get_arg_id(optarg, &arg_id, params.c_str()))
        error(msg_str("unknown player name '%s'", optarg).c_str(), c);
      player.set_name(arg_id);
      break;
    }

    default:
      error("unknown command line error");
    }
  }

  if (argc - optind > 0)
    error(msg_str("invalid option or parameter: '%s'", argv[optind]));

  if (oled == 0)
    error("must specify a 128x64 oled", 'o');

  const int min_spect_width = bars + (bars - 1) * gap; // assume bar width = 1
  if (min_spect_width > SPECT_WIDTH)
    error(msg_str(
        "spectrum graph width is %d: to display %d bars with a gap of %d\n"
        "requires a minimum width of %d. Reduce the number of bars and/or the "
        "gap\n",
        SPECT_WIDTH, bars, gap, min_spect_width));
}

string print_config_file(int bars, int framerate, string cava_method,
                         string cava_source, string fifo_path_cava_out)
{
  char templt[] = "/tmp/cava_config_XXXXXX";
  int fd = mkstemp(templt);
  if (fd == -1)
    return ""; // failed to open file and convert to file stream
  FILE *ofile = fdopen(fd, "w");
  if (ofile == NULL)
    return ""; // failed to open file and convert to file stream

  fprintf(ofile,
          "[general]\n"
          "framerate = %d\n"
          "bars = %d\n"
          "\n"
          "[input]\n"
          "method = %s\n"
          "source = %s\n"
          "\n"
          "[output]\n"
          "method = raw\n"
          "data_format = binary\n"
          "channels = mono\n"
          "raw_target = %s\n"
          "bit_format = 8bit\n",
          framerate, bars, cava_method.c_str(), cava_source.c_str(),
          fifo_path_cava_out.c_str());
  fclose(ofile);
  return templt;
}

// Open the read end ourselves so this call cannot block waiting for cava.
// cava writes the same FIFO. If cava exits, the UI keeps drawing.
Status start_cava(FILE **p_fifo_file, pid_t *cava_pid, const OledOpts &opts)
{
  const string fifo_path_cava_out = msg_str("/tmp/cava_fifo_%d", getpid());
  if (*p_fifo_file == NULL) {
    unlink(fifo_path_cava_out.c_str());
    if (mkfifo(fifo_path_cava_out.c_str(), 0666) == -1)
      opts.error("could not create cava output FIFO for writing: " +
                 string(strerror(errno)));

    int fd = open(fifo_path_cava_out.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
      opts.error("could not open cava output FIFO for reading: " +
                 string(strerror(errno)));
    *p_fifo_file = fdopen(fd, "rb");
    if (*p_fifo_file == NULL)
      opts.error("could not open cava output FIFO for reading");
  }

  if (*cava_pid > 0)
    return Status::ok();

  static string config_file_name;
  if (config_file_name.empty()) {
    config_file_name =
        print_config_file(opts.bars, opts.framerate, opts.cava_method,
                          opts.cava_source, fifo_path_cava_out);
    if (config_file_name.empty())
      opts.error("could not create cava config file: " +
                 string(strerror(errno)));
  }

  pid_t pid = fork();
  if (pid < 0)
    opts.error("could not start cava program: " + string(strerror(errno)));
  if (pid == 0) {
    for (int fd = 3; fd < 256; ++fd)
      close(fd);
    execlp(opts.cava_prog_name.c_str(), opts.cava_prog_name.c_str(), "-p",
           config_file_name.c_str(), (char *)NULL);
    _exit(127);
  }
  *cava_pid = pid;
  return Status::ok();
}

// Idle screen, 128x64:
//   weekday + link      y 0
//   clock, text size 3  y 16
//   date                y 44
//   centered IP         y 56
void draw_clock(ArduiPi_OLED &display, const display_info &disp_info)
{
  display.clearDisplay();
  if (disp_info.alt_layout) {
    // Size 4 is 120x32. Center it on 128x64.
    draw_time(display, 4, 16, 4, disp_info.clock_format);
    return;
  }
  const int W = 6;

  char weekday[16];
  time_t t = time(0);
  struct tm *now = localtime(&t);
  strftime(weekday, sizeof(weekday), "%A", now);
  draw_text(display, 0, 0, 16, weekday);
  draw_connection(display, 128 - 2 * W, 0, disp_info.conn);

  draw_time(display, 19, 16, 3, disp_info.clock_format);
  draw_date(display, 34, 44, 1, disp_info.date_format);

  string ip = disp_info.conn.get_ip_addr();
  int ip_chars = (int)ip.size();
  if (ip_chars > 21)
    ip_chars = 21;
  int ip_x = (128 - ip_chars * W) / 2;
  if (ip_x < 0)
    ip_x = 0;
  draw_text(display, ip_x, 56, 21, ip);
}

// Playing screen, 128x64. Spectrum uses the full width; status is below it
// so bitrate, volume and the clock no longer sit on top of the bars.
//   spectrum 128x30     y 0
//   title               y 32
//   artist / station    y 41
//   elapsed, rate, vol  y 50
//   progress            y 60
void draw_spect_display(ArduiPi_OLED &display, const display_info &disp_info)
{
  if (disp_info.alt_layout) {
    draw_spectrum(display, 0, 0, 128, 64, disp_info.spect, true);
    return;
  }

  const int W = 6;

  draw_spectrum(display, 0, 0, SPECT_WIDTH, 30, disp_info.spect);

  vector<double> scroll_title(disp_info.scroll.begin(),
                              disp_info.scroll.begin() + 2);
  draw_text_scroll(display, 0, 32, 21, disp_info.status.get_title(),
                   scroll_title, disp_info.text_change.secs());

  vector<double> scroll_origin(disp_info.scroll.begin() + 2,
                               disp_info.scroll.begin() + 4);
  draw_text_scroll(display, 0, 41, 21, disp_info.status.get_origin(),
                   scroll_origin, disp_info.text_change.secs());

  int elapsed = disp_info.status.get_elapsed_secs();
  if (elapsed < 0)
    elapsed = 0;
  char elapsed_str[8];
  snprintf(elapsed_str, sizeof(elapsed_str), "%02d:%02d", (elapsed / 60) % 100,
           elapsed % 60);
  draw_text(display, 0, 50, 5, elapsed_str);

  if (disp_info.status.get_kbitrate() > 0)
    draw_text(display, 6 * W, 50, 4, disp_info.status.get_kbitrate_str());

  int vol = disp_info.status.get_volume();
  char vol_str[8];
  if (vol < 0)
    snprintf(vol_str, sizeof(vol_str), " --");
  else {
    if (vol > 100)
      vol = 100;
    snprintf(vol_str, sizeof(vol_str), "%3d", vol);
  }
  draw_text(display, 128 - 6 * W, 50, 3, vol_str);
  draw_connection(display, 128 - 2 * W, 50, disp_info.conn);

  draw_solid_slider(display, 0, 60, 128, 3,
                    100 * disp_info.status.get_progress());
}

void draw_display(ArduiPi_OLED &display, const display_info &disp_info)
{
  mpd_state state = disp_info.status.get_state();
  if (state == MPD_STATE_UNKNOWN || state == MPD_STATE_STOP ||
      (state == MPD_STATE_PAUSE && disp_info.pause_screen == 's'))
    draw_clock(display, disp_info);
  else
    draw_spect_display(display, disp_info);
}

namespace {
pthread_mutex_t disp_info_lock;
}

void *update_info(void *data)
{
  const float delay_secs = 0.3;
  display_info *disp_info_orig = (display_info *)data;
  while (true) {
    pthread_mutex_lock(&disp_info_lock);
    display_info disp_info = *disp_info_orig;
    pthread_mutex_unlock(&disp_info_lock);

    disp_info.status.init(); // Update MPD status info
    disp_info.conn_init();   // Update connection info

    pthread_mutex_lock(&disp_info_lock);
    disp_info_orig->update_from(disp_info);
    pthread_mutex_unlock(&disp_info_lock);

    usleep(delay_secs * 1000000);
  }
};

bool get_invert(double period)
{
  return (period > 0) ? (fmod(time(0) / 3600.0, 2 * period) > period) : period;
}

double saver_cycles_per_min(const OledOpts &opts)
{
  if (opts.saver_speed >= 0)
    return opts.saver_speed;
  if (opts.saver == 1)
    return 0.5;
  if (opts.saver == 3)
    return 60.0;
  return 1.0;
}

// Invert one half of a 128x64 buffer. The boundary passes through the screen
// center and turns clockwise. turns_per_min is full rotations per minute.
void apply_rotating_split(ArduiPi_OLED &display, double turns_per_min)
{
  const int w = display.width();
  const int h = display.height();
  if (w != 128 || h != 64)
    return;

  double theta = 0;
  if (turns_per_min > 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double secs = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
    theta = secs * turns_per_min * (acos(-1.0) * 2.0 / 60.0);
  }
  double nx = -sin(theta);
  double ny = cos(theta);
  const double cx = (w - 1) / 2.0;
  const double cy = (h - 1) / 2.0;

  for (int y = 0; y < h; y++) {
    double dy = (y - cy) * ny;
    for (int x = 0; x < w; x++) {
      if ((x - cx) * nx + dy < 0)
        continue;
      display.xorPixel(x, y);
    }
  }
}

// Invert below a sine boundary. One cycle spans the width, the wave uses the
// full height, and it travels right by widths_per_min screen widths a minute.
void apply_sine_split(ArduiPi_OLED &display, double widths_per_min)
{
  const int w = display.width();
  const int h = display.height();
  if (w != 128 || h != 64)
    return;

  double phase = 0;
  if (widths_per_min > 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double secs = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
    phase = secs * widths_per_min * (acos(-1.0) * 2.0 / 60.0);
  }
  const double pi = acos(-1.0);
  const double cy = (h - 1) / 2.0;

  for (int x = 0; x < w; x++) {
    double bound = cy + cy * sin((2.0 * pi * x) / w - phase);
    int y0 = (int)ceil(bound);
    if (y0 < 0)
      y0 = 0;
    if (y0 > h)
      y0 = h;
    for (int y = y0; y < h; y++)
      display.xorPixel(x, y);
  }
}

struct SaverStripe {
  bool vertical;
  int coord;
  int thickness;
  bool from_low;
  bool white;
  double born;
  int painted;
};

void spawn_stripe(SaverStripe &stripe, int w, int h, int thickness, int axis,
                  int dir, double now)
{
  stripe.vertical = rand() % 2 == 0;
  if (axis == 1)
    stripe.vertical = true;
  else if (axis == 2)
    stripe.vertical = false;
  stripe.from_low = rand() % 2 == 0;
  if (stripe.vertical && dir == 1)
    stripe.from_low = false;
  else if (stripe.vertical && dir == 2)
    stripe.from_low = true;
  else if (!stripe.vertical && dir == 3)
    stripe.from_low = false;
  else if (!stripe.vertical && dir == 4)
    stripe.from_low = true;
  stripe.white = rand() % 2 == 0;
  stripe.born = now;
  stripe.painted = 0;
  stripe.thickness = thickness;
  stripe.coord = rand() % h;
  if (stripe.vertical)
    stripe.coord = rand() % w;
}

void paint_stripe(uint8_t *mask, int w, int h, const SaverStripe &stripe,
                  int from, int to)
{
  int len = h;
  if (!stripe.vertical)
    len = w;
  int cross0 = stripe.coord - stripe.thickness / 2;
  for (int i = from; i < to; i++) {
    int along = i;
    if (!stripe.from_low)
      along = len - 1 - i;
    for (int t = 0; t < stripe.thickness; t++) {
      int cross = cross0 + t;
      int x = along;
      int y = cross;
      if (stripe.vertical) {
        x = cross;
        y = along;
      }
      if (x < 0 || x >= w || y < 0 || y >= h)
        continue;
      mask[y * w + x] = 0;
      if (stripe.white)
        mask[y * w + x] = 1;
    }
  }
}

// The clock or spectrum is drawn first. Stripes only edit a mask: white
// paints inversion, black erases it, and finished stripes stay. A new one
// starts every second. lengths_per_min is how fast that stripe appears.
void apply_stripes(ArduiPi_OLED &display, double lengths_per_min,
                   int thickness, int axis, int dir)
{
  const int w = display.width();
  const int h = display.height();
  if (w != 128 || h != 64 || lengths_per_min <= 0)
    return;
  if (thickness < 1)
    thickness = 1;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
  const double grow = 60.0 / lengths_per_min;
  const double interval = 1.0;

  static uint8_t mask[128 * 64];
  static std::vector<SaverStripe> stripes;
  static double next_at = 0;
  static bool ready = false;
  if (!ready) {
    srand((unsigned)ts.tv_sec);
    next_at = now;
    ready = true;
  }

  if (now >= next_at) {
    SaverStripe stripe;
    spawn_stripe(stripe, w, h, thickness, axis, dir, now);
    stripes.push_back(stripe);
    next_at += interval;
    if (next_at < now)
      next_at = now + interval;
  }

  for (int s = 0; s < (int)stripes.size();) {
    SaverStripe &stripe = stripes[s];
    int len = h;
    if (!stripe.vertical)
      len = w;
    double age = now - stripe.born;
    double cover = 1;
    if (age < grow)
      cover = age / grow;
    if (cover < 0)
      cover = 0;
    int n = (int)(cover * len);
    if (n > len)
      n = len;
    if (n > stripe.painted)
      paint_stripe(mask, w, h, stripe, stripe.painted, n);
    stripe.painted = n;
    if (stripe.painted >= len)
      stripes.erase(stripes.begin() + s);
    else
      s++;
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (mask[y * w + x] == 0)
        continue;
      display.xorPixel(x, y);
    }
  }
}

void noise_pixel(uint8_t *mask, int i, int percent)
{
  mask[i] = 0;
  if (rand() % 100 < percent)
    mask[i] = 1;
}

// Move the sparse mask one pixel. New specks enter on the trailing edge.
void shift_noise(uint8_t *mask, int w, int h, int dir, int percent)
{
  if (dir == 1) {
    memmove(mask, mask + w, (size_t)(h - 1) * w);
    int row = (h - 1) * w;
    for (int x = 0; x < w; x++)
      noise_pixel(mask, row + x, percent);
  }
  else if (dir == 3) {
    for (int y = 0; y < h; y++) {
      memmove(mask + y * w, mask + y * w + 1, (size_t)w - 1);
      noise_pixel(mask, y * w + w - 1, percent);
    }
  }
  else if (dir == 4) {
    for (int y = 0; y < h; y++) {
      memmove(mask + y * w + 1, mask + y * w, (size_t)w - 1);
      noise_pixel(mask, y * w, percent);
    }
  }
  else {
    memmove(mask + w, mask, (size_t)(h - 1) * w);
    for (int x = 0; x < w; x++)
      noise_pixel(mask, x, percent);
  }
}

// Sparse inversion that travels. px_per_sec is how fast the specks move.
// dir: 1 up, 2 down, 3 left, 4 right.
void apply_noise(ArduiPi_OLED &display, double px_per_sec, int percent, int dir)
{
  const int w = display.width();
  const int h = display.height();
  if (w != 128 || h != 64)
    return;

  int pct = percent;
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;

  static uint8_t mask[128 * 64];
  static double carry = 0;
  static double last = 0;
  static bool ready = false;
  if (!ready) {
    srand((unsigned)ts.tv_sec);
    for (int i = 0; i < w * h; i++)
      noise_pixel(mask, i, pct);
    last = now;
    ready = true;
  }

  double dt = now - last;
  if (dt < 0)
    dt = 0;
  if (dt > 0.5)
    dt = 0.5;
  last = now;
  carry += px_per_sec * dt;
  int steps = (int)carry;
  int limit = h;
  if (dir == 3 || dir == 4)
    limit = w;
  if (steps > limit)
    steps = limit;
  carry -= steps;
  for (int s = 0; s < steps; s++)
    shift_noise(mask, w, h, dir, pct);

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (mask[y * w + x] == 0)
        continue;
      display.xorPixel(x, y);
    }
  }
}

void apply_saver(ArduiPi_OLED &display, const OledOpts &opts)
{
  if (opts.saver == 1)
    apply_rotating_split(display, saver_cycles_per_min(opts));
  else if (opts.saver == 2)
    apply_sine_split(display, saver_cycles_per_min(opts));
  else if (opts.saver == 3)
    apply_stripes(display, saver_cycles_per_min(opts), opts.stripe_thickness,
                  opts.stripe_axis, opts.stripe_dir);
  else if (opts.saver == 4) {
    double speed = opts.saver_speed;
    if (speed < 0)
      speed = 30;
    apply_noise(display, speed, opts.noise_percent, opts.noise_dir);
  }
}

int start_idle_loop(ArduiPi_OLED &display, const OledOpts &opts)
{
  const double update_sec =
      1 / (0.9 * opts.framerate); // default update freq just under framerate
  const long select_usec =
      update_sec * 1100000; // slightly longer, but still less than framerate
  Timer timer;

  display_info disp_info;
  disp_info.scroll = opts.scroll;
  disp_info.clock_format = opts.clock_format;
  disp_info.date_format = opts.date_format;
  disp_info.pause_screen = opts.pause_screen;
  disp_info.alt_layout = opts.alt_layout;
  disp_info.spect.init(opts.bars, opts.gap);
  disp_info.status.set_player(opts.player);
  disp_info.status.init();

  // Update MPD info in separate thread to avoid stuttering in the spectrum
  // animation.
  pthread_t update_info_thread;
  if (pthread_create(&update_info_thread, NULL, update_info,
                     (void *)(&disp_info))) {
    fprintf(stderr, "error: could not create pthread\n");
    return 1;
  }

  if (pthread_mutex_init(&disp_info_lock, NULL) != 0) {
    fprintf(stderr, "error: could not create pthread mutex\n");
    return 2;
  }

  // Cava not yet started. fifo_file stays open across cava restarts.
  int fifo_fd = -1;
  FILE *fifo_file = nullptr;
  pid_t cava_pid = -1;
  time_t cava_not_before = 0;

  int zero_read_cnt = 0; // number of consecutive reads of zero bytes
  std::vector<unsigned char> cava_pending;
  while (true) {
    int num_bars_read = 0;
    const int bars_need = (int)disp_info.spect.heights.size();
    if (fifo_fd >= 0) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(fifo_fd, &set);

      // FIFO read timeout value
      struct timeval timeout;
      timeout.tv_sec = 0;
      timeout.tv_usec = select_usec; // slightly longer than timer

      // read() so a short non-blocking read does not stick the FILE error.
      if (select(FD_SETSIZE, &set, NULL, NULL, &timeout) > 0) {
        unsigned char buf[512];
        while (true) {
          ssize_t n = read(fifo_fd, buf, sizeof(buf));
          if (n < 0) {
            if (errno == EINTR)
              continue;
            break;
          }
          if (n == 0)
            break;
          cava_pending.insert(cava_pending.end(), buf, buf + n);
        }
      }
      if (bars_need > 0 && (int)cava_pending.size() >= bars_need) {
        int frames = (int)cava_pending.size() / bars_need;
        int start = (frames - 1) * bars_need;
        for (int i = 0; i < bars_need; i++)
          disp_info.spect.heights[i] = cava_pending[start + i];
        cava_pending.erase(cava_pending.begin(),
                           cava_pending.begin() + frames * bars_need);
        num_bars_read = bars_need;
      }
    }

    if (num_bars_read == 0)
      zero_read_cnt++;
    else
      zero_read_cnt = 0;

    // Clear spectrum data if no data available or music not playing
    if (zero_read_cnt > 1 || disp_info.status.get_state() != MPD_STATE_PLAY) {
      std::fill(disp_info.spect.heights.begin(), disp_info.spect.heights.end(),
                0);
      // A saver has to redraw every frame so the pattern keeps moving.
      if (opts.saver != 0)
        usleep((useconds_t)(update_sec * 1000000.0));
      else
        usleep(0.1 * 1000000); // 0.1 sec delay, don't idle too fast if no need
    }

    // Update display if necessary
    if (timer.finished() || num_bars_read) {
      display.clearDisplay();
      pthread_mutex_lock(&disp_info_lock);
      if (opts.alt_layout || opts.saver != 0)
        display.invertDisplay(false);
      else
        display.invertDisplay(get_invert(opts.invert));
      draw_display(display, disp_info);
      pthread_mutex_unlock(&disp_info_lock);
      apply_saver(display, opts);
      display.display();
    }

    if (timer.finished()) {
      display.reset_offset();
      if (cava_pid > 0) {
        int cava_status = 0;
        if (waitpid(cava_pid, &cava_status, WNOHANG) == cava_pid)
          cava_pid = -1;
      }
      if (disp_info.status.get_state() == MPD_STATE_PLAY && cava_pid < 0) {
        // Give Moode a moment before the first cava start, without blocking
        // the screen. https://github.com/antiprism/mpd_oled/issues/67
        time_t now = time(0);
        if (cava_not_before == 0)
          cava_not_before = now + 2;
        if (now >= cava_not_before) {
          opts.print_status_or_exit(start_cava(&fifo_file, &cava_pid, opts));
          fifo_fd = fileno(fifo_file);
          cava_not_before = now + 2;
        }
      }
      if (disp_info.status.get_state() != MPD_STATE_PLAY)
        cava_not_before = 0;

      timer.set_timer(update_sec); // Reset the timer
    }
  }

  return 0;
}

int main(int argc, char **argv)
{
  // Set locale to allow iconv transliteration to US-ASCII
  setlocale(LC_CTYPE, "C.UTF-8");
  OledOpts opts;
  opts.process_command_line(argc, argv);

  // Set up the OLED doisplay
  if (!init_display(display, opts.oled, opts.i2c_addr, opts.i2c_bus,
                    opts.reset_gpio, opts.spi_dc_gpio, opts.spi_cs,
                    opts.rotate180))
    opts.error("could not initialise OLED");

  init_signals();
  atexit(cleanup);
  int loop_ret = start_idle_loop(display, opts);

  if (loop_ret != 0)
    exit(EXIT_FAILURE);

  return 0;
}
