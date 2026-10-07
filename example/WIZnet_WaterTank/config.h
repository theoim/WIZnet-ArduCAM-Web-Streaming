/**
 * @file    config.h
 * @brief   Everything that has to be decided before this leaves the bench.
 *
 * Three of these are secrets or site facts and one is a guess waiting for a
 * measurement. They are gathered here so that commissioning a unit means
 * editing one file rather than hunting through five.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/* ------------------------------------------------------------- credentials */
/*
 * Discord webhook. Channel settings -> Integrations -> Webhooks -> Copy.
 *
 * This is a password: anyone holding it can post to that channel. It is in a
 * header today because the board has nowhere else to keep it; before this ships
 * it belongs in flash, written through a setup page, so that a repository or a
 * screen share cannot leak it.
 */
/*
 * The three values below are secrets and they are NOT kept in this file.
 *
 * A webhook URL is a password: anyone holding it can post into that Discord
 * channel. The token is what stands between the camera and whoever finds the
 * forwarded port. Both were written here as literals, which put them into every
 * build, every console log, every screen share and - one `git add` away - into
 * the history of a repository, where deleting them later does not remove them.
 *
 * They now live in secrets.h, which is not committed. Copy secrets.h.example to
 * secrets.h and fill it in; the placeholders below are what a build without that
 * file gets, and the firmware says so at boot rather than running on them.
 */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/*
 * No webhook is a working state, not an error: the box still watches, still
 * serves the panel and still records alarms on the console. It simply cannot
 * tell anybody, and it says that at boot. Anything else would mean a board that
 * refuses to start because it cannot talk to a chat service.
 */
#ifndef WEBHOOK_URL
#define WEBHOOK_URL         ""
#endif

/*
 * The token that gates the live view.
 *
 * Make it long and random - this is the only thing standing between the camera
 * and the open internet while the stream is up. A request without it gets 404
 * rather than 401, so a scanner cannot even learn that the path exists.
 */
#ifndef STREAM_TOKEN
#define STREAM_TOKEN        "CHANGEME"
#endif

/*
 * How the phone reaches this board from outside, including the port the router
 * forwards. The board cannot discover this for itself, and it goes into the
 * alert so the link is tappable.
 *
 * Example: "http://222.111.22.33:8080"
 */
#ifndef PUBLIC_URL_BASE
#define PUBLIC_URL_BASE     "http://CHANGEME:8080"
#endif

/* ------------------------------------------------------------------ network */
/*
 * Static, because a port forward points at an address and a lease that moves
 * breaks it. A reservation on the router would do as well, and is tidier.
 */
#define NET_MAC             { 0x00, 0x08, 0xDC, 0x12, 0x34, 0x59 }

/*
 * Ask the router for an address at boot (1), or use the fixed one below (0).
 *
 * DHCP by default, because a box that is carried to a site nobody has measured
 * is a box on an unknown subnet. A fixed 192.168.11.x is unreachable on a
 * 192.168.0.x network, and the panel that would let someone fix that is behind
 * the very address that does not work - so the fixed default locks itself out of
 * the building it was installed in.
 *
 * Two things make DHCP usable even though the address is then unknown: the
 * console prints it, and the startup message on Discord carries it. If there is
 * no DHCP server the board waits up to a minute, says so, and falls back to the
 * fixed address.
 */
#define NET_USE_DHCP        1
#define NET_IP              { 192, 168, 11, 7 }
#define NET_SUBNET          { 255, 255, 255, 0 }
#define NET_GATEWAY         { 192, 168, 11, 1 }
#define NET_DNS             { 8, 8, 8, 8 }

#define HTTP_PORT           80      /* the router maps its external port here */

/*
 * Socket budget. Eight exist and they do not multiply.
 *
 *   7            DNS
 *   0            HTTPS to Discord, opened and closed per message
 *   1 .. 6       web server
 *
 * One listener per concurrent client, because a W6300 listening socket becomes
 * the connection - there is no accept() handing back a fresh one.
 *
 * Six rather than three, because the stream no longer waits for an alarm. A
 * viewer now holds one socket open permanently for the video, the status poll
 * takes another every second, and a socket that has just been closed spends a
 * moment in CLOSE_WAIT before it can listen again. With three, a reload during
 * a stream left nothing to answer with - which is what a refused connection on
 * the forwarded port looks like from a phone.
 */
#define SOCK_DNS            7
#define SOCK_HTTPS          0
#define SOCK_HTTP_BASE      1
#define SOCK_HTTP_COUNT     6

/*
 * DHCP borrows the last web socket.
 *
 * All eight are spoken for, and DHCP does not need one for long: it runs once,
 * before the listeners are opened, and the socket is closed again as soon as an
 * address is in hand. Borrowing beats reserving a ninth that does not exist.
 */
#define SOCK_DHCP           (SOCK_HTTP_BASE + SOCK_HTTP_COUNT - 1)

/*
 * Shortest gap between two streamed frames, in milliseconds.
 *
 * With the stream always running this is the difference between a monitor and a
 * load generator, and at HD it is also the difference between hearing the buzzer
 * and not. Each frame blocks the loop for about 100 ms, and whatever share of
 * the second goes into frames does not go into audio: at 200 ms the microphone
 * was down to 24 blocks a second out of 62.
 *
 * It was 500 ms, and that number was a single-core compromise: every frame was
 * audio the microphone did not get, so two a second was as much as could be
 * afforded. Moving the camera to core 1 removed that argument entirely - the
 * measured overrun count during streaming is now zero.
 *
 * 100 ms is not a promise of ten frames a second. It is a floor low enough to
 * stop being the limit, so that whatever the camera and the link can actually
 * do is what shows up in the [cam] line. Set it from that measurement rather
 * than from hope.
 */
#define STREAM_MIN_INTERVAL_MS  100

/*
 * TCP keepalive, in units of 5 seconds, applied to each web connection.
 *
 * A phone that walks out of Wi-Fi does not close its connection; the socket
 * simply stops answering, and without this the chip holds it open indefinitely
 * while the stream writes into a buffer nobody is draining. Two units is ten
 * seconds, after which the chip drops it and the listener goes back into the
 * pool.
 */
#define KEEPALIVE_UNITS     2

/* ------------------------------------------------------------------ detector */
/*
 * Provisional, and knowingly so.
 *
 * Measured on the bench: a quiet room reaches 11.8, shouting into the
 * microphone reached 15.4, and a steady 2 kHz tone read 184 to 782. So 25
 * clears the worst false positive by 1.6x and sits far below any real tone.
 *
 * It is still a bench number. The value that ships should come from the buzzer
 * in that basement measured against that basement's own noise - which is why
 * the scan covers a range instead of one frequency, so the first alarm reports
 * which bin fired and the range can be narrowed afterwards.
 */
/*
 * Where the detector starts, before anybody adjusts it from the panel.
 *
 * Measured against the real sounder: the room read 0.004 to 0.008 and the
 * buzzer 0.012 to 0.016, so 2.2 times the learned background sits above the
 * room's own peaks and below the buzzer's floor. The tonal threshold of 20 was
 * chosen the same way, between a shouted voice at 15 and a tone at 23 and up.
 *
 * Both are starting points. The values that ship to a site should come from
 * that site's own numbers, read off the panel while its buzzer sounds.
 */
#define DETECT_LOUD_K       2.2f
#define DETECT_ENTER_RATIO  20.0f

#define SCAN_LO_HZ          1500.0f
#define SCAN_HI_HZ          4500.0f

/*
 * A second scan, below the first, that only writes to the log.
 *
 * The detector does not read it. It exists because the band above was chosen
 * for a piezo sounder and the thing in the panel may not be one: when the real
 * buzzer sounded, all 49 bins of 1500-4500 Hz lifted together and the tone
 * ratio stayed near 5, which is what a band sees when the energy that matters
 * is somewhere else and only its skirts reach in. The alarm then fired after the
 * buzzer stopped, on a faint clean 2062 Hz at room level - a harmonic or an
 * after-ring, not the sound itself.
 *
 * So the question is where the fundamental actually is, and no setting of the
 * band above can answer it. 312.5 Hz to 1437.5 Hz is 19 bins at the same 62.5 Hz
 * width, covering the range panel sounders are usually built in. One of the two
 * bars will have a column in it.
 *
 * Both ends are multiples of the bin width on purpose: a bin centre that falls
 * between two Goertzel frequencies splits one tone across two columns and reads
 * as less tonal than it is.
 */
/*
 * The per-second microphone line. 1 prints it, 0 keeps the console for events.
 *
 * Off for normal running: a line a second scrolls everything else away, and the
 * things worth seeing - an address, an alarm, a socket closing - are the things
 * that scroll. It stays in the build because the next question about this box
 * will be a question about numbers, and it is one digit to get them back.
 */
#define MIC_DEBUG_LOG       0

#define SCAN_LOW_LO_HZ      312.5f
#define SCAN_LOW_HI_HZ      1437.5f

/*
 * Thirty seconds of silence to stand down, against five on the bench.
 *
 * It has to outlast the longest gap a buzzer leaves between beeps. Shorter and
 * the alarm clears between two beeps of the same event, re-arms, and sends
 * another alert - the owner gets a message per beep.
 */
#define CLEAR_SECONDS       15.0f

/*
 * Camera resolution a brand-new board starts at (SETTINGS_RES_* in settings.h):
 * 1 is QVGA 320x240, 2 is VGA 640x480, 3 is HD 1280x720.
 *
 * HD, because the picture has to be good enough to read a water level off and
 * 320x240 is not. The earlier default of QVGA was argued from the uplink - get
 * any picture through first, raise it later - and that cost more than it bought:
 * a 40 KB frame crosses a domestic uplink without trouble, and what someone does
 * with a blurry one is stop trusting the box.
 */
#define CAM_RES_DEFAULT     3

/* ------------------------------------------------------------------- camera */
/*
 * The live view stays open this long after the alarm clears.
 *
 * Long enough to walk to a phone and look; short enough that the port is not
 * sitting open on a public address for the rest of the day.
 */
#define STREAM_LINGER_SEC   300     /* five minutes */

/* MJPEG part separator. Any string the JPEG cannot contain. */
#define MJPEG_BOUNDARY      "wiznetframe"

/* ---------------------------------------------------------------- platform */
#define PLL_SYS_KHZ         (200 * 1000)

/* I2S microphone. GP2/GP3 must stay adjacent: PIO drives both clocks from a
 * two-bit side-set. GP4 is the camera's VSYNC and GP5..GP14 its data. */
#define PIN_I2S_BCLK        2
#define PIN_I2S_DATA        28

/*
 * Two indicators on the control box.
 *
 * RUN is a heartbeat: it blinks while the firmware is running its loop, which
 * is the one thing a panel light can say that a steady lamp cannot. A steady
 * lamp only proves the board has power.
 *
 * BUZZER follows the detector. Below the threshold it glows in proportion to
 * how close the sound is to alarming, and goes solid when it does - so someone
 * standing in front of the box can see a near miss, which is exactly what
 * matters while the threshold is still being settled on site.
 *
 * GP26 is the last free pin on this board. GP25 is the on-board LED and is
 * given to BUZZER because that is the one being tested.
 */
#define PIN_LED_BUZZER      25
#define PIN_LED_RUN         26

#endif /* __CONFIG_H__ */
