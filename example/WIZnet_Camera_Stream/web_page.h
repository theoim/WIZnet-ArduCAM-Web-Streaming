/**
 * @file    web_page.h
 * @brief   Embedded single-page UI for the exhibition build.
 *
 * The whole UI is one self-contained string: no external CSS, JS, fonts or
 * images, because on a show floor the device is often the only server the
 * browser can reach.
 *
 * Both firmware images serve this same page. The badge and the accent colour
 * come from the "stack" field of /api/status, so the two boards can sit side by
 * side on one screen and be told apart across a room.
 *
 * What this adds over example/WIZnet_ArduCAMMega_TOE_Web_Streaming:
 *
 *   - A load generator. One connection pulling filler while the stream runs, so
 *     a visitor can put the two stacks under the same pressure and watch which
 *     one gives way. This is the exhibit.
 *   - Three states instead of two. "Slow because it is loaded" and "not
 *     answering" used to look identical - a frozen picture - and at an
 *     exhibition that reads as a broken product either way. They are now
 *     separate states with separate colours and separate wording.
 *   - A stream that comes back on its own. Pull the cable and the old page
 *     stayed frozen after it was plugged back in, because a broken multipart
 *     response is not retried by the browser and img.src was only ever set on
 *     START. The device reports its frame counter; if that stops moving while
 *     it still says it is streaming, nothing is arriving, so ask again.
 *   - A sensor panel built from what the device reports (/api/controls), so a
 *     row added in cam_controls.c appears here with no edit to this file.
 *
 * HTML attributes use single quotes so the C string needs no escaping.
 */

#ifndef WEB_PAGE_H
#define WEB_PAGE_H

/* EXHIBITION_SIMPLE_UI lives here. The page is one C string, so the switch
   is applied by the preprocessor between adjacent string literals: the block
   it guards simply is not part of the string in the other mode. */
#include "stream_config.h"

static const char HTTP_INDEX_PAGE[] =
"<!DOCTYPE html>\n"
"<html lang='en'>\n"
"<head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>WIZnet Camera Stream</title>\n"
/* An empty data: icon, purely to stop the browser asking for /favicon.ico.
   That request is one more TCP connection during the busiest second this
   device has - the page, the logo, the control list, the first status poll and
   the stream are all in flight at the same moment - and it exists only to be
   answered 404. Thirty bytes here buys a connection back. */
"<link rel='icon' href='data:,'>\n"
"<style>\n"
":root{\n"
"  --bg:#ffffff; --panel:#ffffff; --line:#dfe5ec;\n"
"  --text:#101418; --muted:#69737f;\n"
/* Red carries emphasis; the buttons and the chart lines keep their own colours
   so the two boards stay comparable against earlier screenshots. */
"  --accent:#e01b24; --ok:#3ddc84; --stop:#ff5c5c; --warn:#f5a623;\n"
"  --load:#2f6fed;\n"
/* Set from the status response: one colour per network stack, so which board
   is which is readable across a room without reading any text. */
"  --stack:#e01b24;\n"
"}\n"
"*{box-sizing:border-box}\n"
/* hidden must win over any display rule (#authbox is display:flex): without
   this the login card stayed on screen after a successful login. */
"[hidden]{display:none!important}\n"
"body{margin:0;background:var(--bg);color:var(--text);\n"
"  font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif}\n"

/* --- Header: sized for a screen seen from a few metres away --- */
"header{display:flex;align-items:center;gap:18px;padding:12px 24px;\n"
"  background:var(--panel);border-bottom:3px solid var(--stack)}\n"
"header .logo{height:52px;display:block}\n"
"header .sub{color:var(--muted);font-size:13px}\n"
"header .sub b{color:var(--text);font-weight:600}\n"
"header .stackbox{margin-left:auto;display:flex;align-items:center;gap:16px}\n"
"header .tag{padding:6px 30px;border-radius:8px;background:var(--stack);\n"
"  color:#ffffff;font-weight:800;font-size:38px;line-height:1.1;\n"
"  letter-spacing:3px;min-width:200px;text-align:center}\n"
"header .what{color:var(--muted);font-size:13px;max-width:210px;\n"
"  line-height:1.4;text-align:right}\n"
".rate{display:none}\n"

/* --- The state banner. The whole point of this page having three states
       instead of two, so it gets a full-width strip rather than a word in a
       list: at a booth nobody reads a definition list. --- */
"#banner{display:none;padding:10px 24px;font-size:14px;font-weight:600;\n"
"  color:#08121c;text-align:center}\n"
"#banner.show{display:block}\n"
"#banner.loaded{background:var(--load);color:#ffffff}\n"
"#banner.gone{background:var(--warn);animation:blink 1s steps(2,end) infinite}\n"
"#banner.stall{background:var(--stop);color:#ffffff}\n"
"@keyframes blink{50%{opacity:.25}}\n"

/* Video gets the room; everything else is reference material beside it. */
/* --- Login (Phase 3b): who is logged in, and the login/setup card --- */
"header .who{margin-left:auto;display:flex;align-items:center;gap:10px;\n"
"  font-size:13px;color:var(--muted)}\n"
/* Opaque: nothing of the camera shows behind the login card. */
"#authbox{position:fixed;inset:0;z-index:20;background:var(--bg);\n"
"  display:flex;align-items:center;justify-content:center;padding:16px}\n"
"#authbox form{width:100%;max-width:340px;display:flex;flex-direction:column;gap:10px}\n"
"#authbox input,.acard input{width:100%;padding:8px;border-radius:7px;\n"
"  border:1px solid var(--line);font-size:14px}\n"
"#authbox label{margin:0}\n"
".acards{display:grid;grid-template-columns:1fr 1fr;gap:16px}\n"
"@media(max-width:800px){.acards{grid-template-columns:1fr}}\n"
".acard{border:1px solid var(--line);border-radius:10px;padding:14px;\n"
"  display:flex;flex-direction:column;gap:8px;min-width:0}\n"
".acard h3{margin:0;font-size:15px}\n"
".acard .desc{margin:0;font-size:13px;color:var(--muted);line-height:1.45}\n"
".acard .id{font-size:13px}\n"
".acard .id b{font-family:ui-monospace,Consolas,monospace}\n"
".acard label{margin:4px 0 0;font-size:12px}\n"
".acard .row2{display:flex;gap:8px;flex-wrap:wrap;margin-top:6px}\n"
"#sec-acct .hint{margin-top:10px}\n"
"#sec-acct{order:10}                    /* below the camera panels */\n"
/* A result worth seeing from across the room: centred over the picture. */
"#toast{position:absolute;top:56px;left:50%;transform:translateX(-50%);z-index:3;\n"
"  padding:10px 22px;border-radius:10px;font-size:20px;font-weight:800;\n"
"  color:#ffffff;background:rgba(16,20,24,.8);white-space:nowrap;max-width:90%;\n"
"  overflow:hidden;text-overflow:ellipsis}\n"
"#toast.ok{background:#1e9e4a}\n"
"#toast.bad{background:var(--stop)}\n"
"body.is-viewer #sec-ctrls,body.is-viewer #sec-res{display:none!important}\n"
"main{max-width:1500px;margin:0 auto;padding:16px;display:grid;\n"
"  grid-template-columns:minmax(0,1fr) 250px;gap:16px}\n"
"@media(max-width:960px){main{grid-template-columns:1fr}}\n"
".card{background:var(--panel);border:1px solid var(--line);\n"
"  border-radius:10px;padding:14px;box-shadow:0 1px 3px rgba(16,20,24,.06)}\n"
".wide{grid-column:1/-1}\n"
".card h2{margin:0 0 10px;font-size:12px;text-transform:uppercase;\n"
"  letter-spacing:1.5px;color:var(--muted);font-weight:600}\n"
".card.tight{padding:12px}\n"

/* No flex on .view: a flex item defaults to min-width:auto, so an image whose
   natural width exceeds the column overflows instead of scaling down. */
".view{background:#000;border-radius:8px;overflow:hidden;min-height:240px}\n"
".view img,.view canvas{display:block;width:100%;max-width:100%;height:auto}\n"

/* --- Buttons: solid for the two actions that change state, ghost for the
       rest, so the exhibition screen has one obvious pair of controls. --- */
".btns{display:flex;gap:8px;margin-bottom:12px}\n"
"button{padding:9px 12px;border-radius:7px;cursor:pointer;font-size:13px;\n"
"  font-weight:600;letter-spacing:.5px;transition:background .15s,color .15s}\n"
"button:disabled{opacity:.3;cursor:not-allowed}\n"
".btns button{flex:1;border:0;color:#08121c}\n"
"#start{background:var(--ok)} #stop{background:var(--stop)}\n"
".btns button:not(:disabled):hover{filter:brightness(1.1)}\n"
"button.ghost{background:transparent;border:1px solid var(--line);\n"
"  color:var(--muted);font-size:11px;padding:7px 12px;text-transform:uppercase}\n"
"button.ghost:hover{border-color:var(--accent);color:var(--accent)}\n"
/* The one action of a panel: filled, so it is found without reading labels. */
"button.primary{background:var(--accent);border:1px solid var(--accent);color:#ffffff;\n"
"  padding:8px 16px}\n"
"button.primary:not(:disabled):hover{filter:brightness(1.1)}\n"
"button.warn{background:transparent;border:1px solid var(--stop);color:var(--stop);\n"
"  padding:8px 14px}\n"

/* --- Load: a segmented row rather than a slider. A visitor should be able to
       hit it once and see something change, and discrete steps make two boards
       comparable - both are on 64, not both somewhere near 64. --- */
".seg{display:flex;gap:0;margin-top:4px}\n"
".seg button{flex:1;border:1px solid var(--line);background:transparent;\n"
"  color:var(--muted);border-radius:0;font-size:13px;padding:10px 0}\n"
".seg button:first-child{border-radius:7px 0 0 7px}\n"
".seg button:last-child{border-radius:0 7px 7px 0}\n"
".seg button+button{border-left:0}\n"
".seg button.on{background:var(--load);border-color:var(--load);\n"
"  color:#ffffff;font-weight:700}\n"

/* --- Resolution bar. Built like the load row rather than as a <select>:
       a dropdown hides four of the five choices until it is opened, and a
       visitor who has to open something first will not touch it at all.
       Named by mode (HD, FHD) rather than by pixels - "1280 x 720" is a
       number to decode, "HD" is already understood. --- */
".resseg{display:flex;gap:0}\n"
".resseg button{flex:1;border:1px solid var(--line);background:transparent;\n"
"  color:var(--muted);border-radius:0;font-weight:700;padding:10px 0;\n"
"  letter-spacing:1px}\n"
".resseg button:first-child{border-radius:7px 0 0 7px}\n"
".resseg button:last-child{border-radius:0 7px 7px 0}\n"
".resseg button+button{border-left:0}\n"
".resseg button small{font-size:.72em;font-weight:600;opacity:.75;\n"
"  letter-spacing:0}\n"
".resseg button.on{background:var(--stack);border-color:var(--stack);\n"
"  color:#ffffff}\n"

/* The mode shown over the picture. Hidden in the full UI, where the same
   value is already a row in the definition list beside it. */
".resnow{display:none}\n"

"select{width:100%;padding:8px;border-radius:7px;border:1px solid var(--line);\n"
"  background:var(--bg);color:var(--text);font-size:13px}\n"
"label{display:block;font-size:11px;color:var(--muted);margin:0 0 5px}\n"
"dl{display:grid;grid-template-columns:auto 1fr;gap:7px 10px;margin:12px 0 0;font-size:13px}\n"
"dt{color:var(--muted)}\n"
"dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}\n"
".dot{display:inline-block;width:8px;height:8px;border-radius:50%;\n"
"  margin-right:7px;background:var(--muted);vertical-align:middle}\n"
".dot.live{background:var(--ok);box-shadow:0 0 8px var(--ok)}\n"
".dot.loaded{background:var(--load);box-shadow:0 0 8px var(--load)}\n"
/* Blinking, because a still amber dot beside a still picture is another way of
   showing nothing. */
".dot.gone{background:var(--warn);animation:blink 1s steps(2,end) infinite}\n"
".dot.stall{background:var(--stop);animation:blink 1s steps(2,end) infinite}\n"
"footer{text-align:center;color:var(--muted);font-size:12px;padding:8px 0 24px}\n"

/* --- Charts: frame rate on the left, what it costs the link on the right --- */
".charts{display:grid;grid-template-columns:1fr 1fr;gap:24px}\n"
"@media(max-width:900px){.charts{grid-template-columns:1fr}}\n"
".chead{display:flex;align-items:flex-end;gap:16px;margin-bottom:6px;flex-wrap:wrap}\n"
".chead .big{font-size:40px;font-weight:700;line-height:1;\n"
"  font-variant-numeric:tabular-nums}\n"
".chead .big small{font-size:14px;color:var(--muted);margin-left:8px;font-weight:400}\n"
".chead .stats{display:flex;gap:16px;margin-left:auto;font-size:12px;\n"
"  color:var(--muted)}\n"
".chead .stats b{color:var(--text);font-variant-numeric:tabular-nums}\n"
".chead .stats b.load{color:var(--load)}\n"
".legend i.sram{background:var(--muted)}\n"
"canvas{display:block;width:100%;height:180px}\n"
".legend{display:flex;flex-wrap:wrap;gap:20px;margin-top:10px;\n"
"  font-size:13px;color:var(--muted)}\n"
".legend i{display:inline-block;width:10px;height:10px;border-radius:2px;\n"
"  margin-right:7px;vertical-align:middle}\n"
".legend b{color:var(--text);font-variant-numeric:tabular-nums}\n"
"i.vsync{background:var(--warn)} i.read{background:var(--accent)}\n"
"i.drain{background:#00b8d4}\n"
"i.send{background:#8e7bef}\n"
".hint{margin:12px 0 0;font-size:13px;color:var(--muted);line-height:1.5}\n"
".hint b{color:var(--accent)}\n"

/* --- Sensor clock: one compact row, it is a tuning aid not the exhibit --- */
".sweep{display:flex;flex-wrap:wrap;gap:14px;align-items:center}\n"
".sweep>div{flex:1;min-width:180px;display:flex;align-items:center;gap:10px}\n"
".sweep label{margin:0;white-space:nowrap;font-size:11px}\n"
".sweep label b{color:var(--accent);font-variant-numeric:tabular-nums}\n"
"input[type=range]{flex:1;min-width:90px;accent-color:var(--accent)}\n"

/* --- Sensor controls: groups side by side, each a stack of rows. Built from
       /api/controls, so the layout has to survive whatever the device reports
       rather than assuming a fixed set. --- */
".ctrls{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));\n"
"  gap:10px 24px}\n"
".grp h3{margin:0 0 8px;font-size:11px;text-transform:uppercase;\n"
"  letter-spacing:1.2px;color:var(--accent);font-weight:700}\n"
".row{display:flex;align-items:center;gap:10px;margin-bottom:7px;font-size:12px}\n"
".row .nm{color:var(--muted);flex:0 0 96px}\n"
".row .vl{color:var(--text);font-variant-numeric:tabular-nums;\n"
"  flex:0 0 34px;text-align:right}\n"
".row input[type=range]{margin:0}\n"
".row input[type=checkbox]{accent-color:var(--accent);width:16px;height:16px;\n"
"  cursor:pointer}\n"
".hint.bad{color:var(--stop)}\n"
".saverow{display:flex;align-items:center;gap:12px;margin-top:12px}\n"
".saverow .hint{margin:0}\n"

/* ------------------------- Simplified exhibition UI -------------------------

   Same markup, same script. Only what gets painted changes, so switching back
   is one #define and not a merge.

   Written for the show laptop, which is lower resolution than the monitor the
   full page was built on. The rule that drives every number below: the picture
   and both graphs must be on screen together without scrolling, because the
   exhibit is the relationship between them - the stream rate falling is only
   worth anything if the picture stuttering is visible at the same moment.

   The picture is not given a size. It is given whatever is left after the
   header and the graph row have taken theirs, which is the only version of
   this that survives an unknown laptop: no vh guess to retune, and a banner
   appearing pushes the picture down by its own height instead of off the
   bottom of the screen.

   A 16:9 frame at the full width of a 1366-wide screen is 760px tall - the
   whole screen - so on a short screen it is the height that binds and the
   frame sits in a black box with bars either side. The box is black, so the
   bars read as the edge of the screen rather than as empty page. --------- */
#if EXHIBITION_SIMPLE_UI
/* One screen, laid out top to bottom: header, picture, graphs. The picture
   is the flexible row, so it absorbs the slack and nothing else has to be
   measured. min-height stops it collapsing to nothing on a very short
   window - past that point the page scrolls, which is the right failure. */
"body{min-height:100vh;display:flex;flex-direction:column}\n"
/* margin:0 matters. The base rule centres main with "margin:0 auto", and an
   auto margin on a stretched flex item makes it shrink to its content and sit
   in the middle - the picture came out two thirds of the screen wide with
   white down both sides. */
"main{flex:1 1 auto;min-height:0;max-width:none;margin:0;padding:8px;gap:8px;\n"
"  grid-template-columns:1fr;grid-template-rows:minmax(200px,1fr) auto auto}\n"
/* Logo over badge, both centred. A visitor reads top to bottom from a few
   metres away and the badge is what they are meant to come away with, so it
   sits directly under the logo with nothing competing beside it. */
/* No longer a booth exhibit, so no stack badge and no graphs: logo on the
   left, frame rate and bandwidth as plain numbers on the right. */
"header{padding:8px 16px;border-bottom-width:2px}\n"
"header .logo{height:34px}\n"
"header .stackbox,#sec-perf{display:none}\n"
"header .stackbox{margin-left:0;flex-direction:column;align-items:center;\n"
"  gap:3px}\n"
"header .tag{order:1;font-size:40px;padding:4px 32px;letter-spacing:2px;\n"
"  min-width:0;border-radius:10px}\n"
/* Both captions go. "Hardwired TCP/IP - the WIZnet chip terminates TCP" is a
   sentence, and nobody walking past a booth reads a sentence - the badge is
   what lands, and a line of explanation under it only competes with it. The
   device address is for whoever set the board up, and they already know it.

   Worth about 50px of header, which goes straight into the picture. */
"header .what,header .sub{display:none}\n"
/* Everything that needs a decision. The board autostarts and the visitor is
   not meant to change the run, so none of it should be reachable.

   The resolution bar joined this list for the OV2640 build. It was the last
   control a visitor could still touch, and on this sensor every press rewrites
   around forty registers and can switch the array between its SVGA and UXGA
   modes - which is where a day of faults came from: recovery loops, a frame
   rate that depended on which mode had been selected before it, and at
   1600x1200 a board that stopped answering the page at all.

   There is also nothing to choose. 800x600 is the fastest mode this sensor has
   at a size worth projecting, the only one read natively rather than scaled
   down from UXGA, and it ran a thousand frames without dropping one. Every
   other mode is slower, and the ones below it are smaller as well, so they put
   less on the link rather than more - the wrong direction for an exhibit about
   what happens to a stack under load.

   What is left on screen is the exhibit itself: two boards, the same picture,
   one number each. 17 against 8. */
"#sec-ctl,#sec-sweep,#sec-ctrls,footer{display:none}\n"
"body.show-ctrls #sec-ctrls{display:block}\n"
"#sec-acct{display:none}\n"
"body.show-ctrls #sec-acct{display:block}\n"

/* Three rows now: picture takes the slack, bar and graphs take what they
   need. The bar is a bare strip, not a card - it reads as part of the
   picture frame rather than as another panel. */
"#sec-res{padding:0;border:0;background:transparent;box-shadow:none}\n"
".resbar{display:flex;align-items:center;justify-content:center;gap:10px}\n"
".resbar label{margin:0;font-size:13px}\n"
".resbar select{width:auto;min-width:200px;font-size:15px}\n"

/* Mirrors the STREAMING pill on the other corner, one size up because it is
   the only thing on the page that answers "what am I looking at". */
"#sec-view .ovr{position:absolute;top:8px;right:10px;z-index:2;display:flex;\n"
"  align-items:center;gap:8px}\n"
".resnow{display:block;padding:5px 16px;border-radius:999px;\n"
"  background:var(--stack);color:#ffffff;font-size:21px;font-weight:800;\n"
"  letter-spacing:1.6px}\n"
"#sec-view .rate{display:flex;gap:14px;padding:6px 14px;border-radius:8px;\n"
"  background:rgba(0,0,0,.75);color:#ffffff;font-size:13px}\n"
"#sec-view .rate b{font-size:20px;font-weight:700;font-variant-numeric:tabular-nums}\n"
/* The picture is its own frame: no card padding, no border, black to the
   edge. STREAMING becomes a pill over the top-left corner instead of a line
   above the picture, which is ~30px of height back for the graphs. */
"#sec-view{position:relative;padding:0;border:0;background:#000;\n"
"  border-radius:8px;overflow:hidden}\n"
"#sec-view h2{position:absolute;top:8px;left:10px;z-index:2;margin:0;\n"
"  padding:4px 12px;border-radius:999px;background:rgba(0,0,0,.55);\n"
"  color:#ffffff;font-size:11px}\n"
/* The picture is taken out of the flow.

   Left in it, the sizing is circular: the grid row is 1fr of the space left
   over, but "left over" is computed from the content, and the content is a
   720px-tall frame asking to be 720px tall. The row resolved to the frame and
   the graphs went off the bottom of the screen.

   Absolute inside a relative section means the frame contributes no height at
   all, so the row is free to be whatever is left, and the frame then fills
   that box. min-height:0 on the section is what lets a grid item shrink below
   its content in the first place. */
"#sec-view{min-height:0;overflow:hidden}\n"
"#sec-view .view{position:absolute;top:0;right:0;bottom:0;left:0;\n"
"  background:#000;border-radius:0}\n"
/* width:auto with a height cap, not width:100% - the frame keeps its aspect
   ratio and stops growing before it pushes the graphs off the screen. */
/* Full width first, height capped second. width:100% makes the picture
   reach both edges of the laptop, which is what the exhibit wants; the
   max-height stops a 16:9 frame from being 760px tall on a 768px screen
   and pushing the graphs off the bottom. When the cap bites, object-fit
   letterboxes inside a black box, so the bars are invisible - and on a
   taller screen the cap never bites and the frame really is edge to edge.
   No fixed height: the box has to be free to shrink for a 4:3 mode. */
"#sec-view .view img,#sec-view .view canvas{width:100%;height:100%;object-fit:contain}\n"
/* Graphs: the numbers stay large, the traces lose the height the picture
   took. 130px still shows the shape of a drop, which is all it has to do. */
"#sec-perf{padding:10px 14px}\n"
"#sec-perf h2{display:none}\n"
".charts{gap:18px}\n"
".chead .big{font-size:34px}\n"
/* 110px still shows the shape of a drop, which is all the trace has to do -
   the number above it is what a visitor actually reads. */
"canvas{height:110px}\n"
".legend{font-size:11px}\n"
".hint{font-size:11px}\n"
#endif /* EXHIBITION_SIMPLE_UI */

"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <img class='logo' src='/logo.png' alt='WIZnet'>\n"
"  <div class='sub'>ArduCAM MEGA &middot; <b id='addr'></b></div>\n"
"  <div class='stackbox'>\n"
"    <div class='what' id='what'>&nbsp;</div>\n"
"    <div class='tag' id='stack'>&nbsp;</div>\n"
"  </div>\n"
"  <div class='who' id='who' hidden><span id='who-name'></span>"
"<button id='logout' class='ghost'>Log out</button></div>\n"
"</header>\n"
"<div id='banner'></div>\n"
"<div id='authbox' hidden><form id='authform' class='card' autocomplete='on'>\n"
"  <h2 id='auth-title'>Log in</h2>\n"
"  <p id='auth-text' class='hint'></p>\n"
"  <label for='a-user'>User</label><input id='a-user' autocomplete='username'>\n"
"  <label for='a-pass'>Password</label>"
"<input id='a-pass' type='password' autocomplete='current-password'>\n"
"  <div id='a-pass2-w' hidden><label for='a-pass2'>Repeat password</label>"
"<input id='a-pass2' type='password' autocomplete='new-password'></div>\n"
"  <button id='a-go' class='primary'>Log in</button>\n"
"  <p id='auth-msg' class='hint bad'></p>\n"
"</form></div>\n"
"<main>\n"
"  <section class='card' id='sec-view'>\n"
"    <h2><span id='dot' class='dot'></span><span id='s-state'>-</span></h2>\n"
/* Which mode is running, over the top-right of the picture, mirroring the
   STREAMING pill on the left. Its text comes from the device reply, not from
   the click, so it shows what the camera actually did. */
/* Frame rate and what the stream puts on the link, beside it. Only painted in
   the simplified layout, where the graphs are gone. */
"    <div class='ovr'>"
"<div class='rate'><span><b id='h-fps'>-</b> fps</span>"
"<span><b id='h-bw'>-</b> Mbps</span></div>"
"<div class='resnow' id='resnow'>-</div></div>\n"
"    <div id='toast' hidden></div>\n"
/* No src here on purpose. With one, the browser opens the stream while this
   page is still being transferred, so the two compete for the same send path -
   and if the device is not streaming yet, that connection sits there holding a
   pcb and producing nothing. The stream is opened from script once the first
   status reply says the device is actually sending frames. */
"    <div class='view'><canvas id='view' role='img' aria-label='camera stream'></canvas></div>\n"
"  </section>\n"
"  <section class='card' id='sec-ctl'>\n"
"    <h2>Control</h2>\n"
"    <div class='btns'>\n"
"      <button id='start'>START</button>\n"
"      <button id='stop'>STOP</button>\n"
"    </div>\n"
"    <label for='res'>Resolution</label>\n"
"    <select id='res'>\n"
/* What an OV2640 actually has. There is no 1280x720 and no 1920x1080 - the
   sensor steps 640x480, 800x600, 1024x768, 1280x1024, 1600x1200 - and the
   entries for those two did nothing at all, because /api/res rejects a size
   the driver has no table for. */
"      <option value='320x240'>320 x 240</option>\n"
"      <option value='640x480'>640 x 480</option>\n"
"      <option value='800x600'>800 x 600</option>\n"
"      <option value='1024x768'>1024 x 768</option>\n"
"      <option value='1280x1024'>1280 x 1024</option>\n"
"      <option value='1600x1200'>1600 x 1200</option>\n"
"    </select>\n"
"    <label style='margin-top:14px'>Link load</label>\n"
"    <div class='seg' id='seg'>\n"
"      <button data-kb='0' class='on'>OFF</button>\n"
"      <button data-kb='32'>32</button>\n"
"      <button data-kb='64'>64</button>\n"
"      <button data-kb='128'>128</button>\n"
"    </div>\n"
"    <dl>\n"
"      <dt>Resolution</dt><dd id='s-res'>-</dd>\n"
"      <dt>Frames</dt><dd id='s-frames'>-</dd>\n"
"      <dt>Dropped</dt><dd id='s-drop'>-</dd>\n"
"      <dt>Load</dt><dd id='s-loadkb'>off</dd>\n"
"    </dl>\n"
"  </section>\n"
/* The resolution bar sits AFTER the control card on purpose. In the full UI
   that keeps the picture and the control column side by side and drops this
   underneath them; in the simplified UI the control card is hidden, so the
   bar lands directly between the picture and the graphs, which is where it
   is wanted. One DOM, two layouts, no duplicated markup.

   The data-res values are the same strings /api/res takes and the same ones
   the select carries, so nothing has to translate between them. */
"  <section class='card wide' id='sec-res'>\n"
"    <div class='resbar'><label for='resbar'>Resolution</label>"
"<select id='resbar'></select></div>\n"
"  </section>\n"
"  <section class='card wide' id='sec-perf'>\n"
"    <h2>Live performance</h2>\n"
"    <div class='charts'>\n"
"      <div>\n"
"        <div class='chead'>\n"
"          <div class='big'><span id='s-fps'>0.0</span><small>fps</small></div>\n"
"          <div class='stats'>\n"
"            <span>min <b id='s-min'>-</b></span>\n"
"            <span>avg <b id='s-avg'>-</b></span>\n"
"            <span>max <b id='s-max'>-</b></span>\n"
"          </div>\n"
"        </div>\n"
"        <canvas id='c-fps'></canvas>\n"
"      </div>\n"
"      <div>\n"
"        <div class='chead'>\n"
"          <div class='big'><span id='s-bw'>0.0</span><small>Mbps stream</small></div>\n"
"          <div class='stats'>\n"
"            <span>frame <b id='s-kb'>0</b> KB</span>\n"
"            <span>load <b id='s-load' class='load'>0.0</b> Mbps</span>\n"
"            <span>peak <b id='s-bwmax'>-</b></span>\n"
"          </div>\n"
"        </div>\n"
"        <canvas id='c-bw'></canvas>\n"
"      </div>\n"
"    </div>\n"
"    <div class='legend'>\n"
"      <span><i class='vsync'></i>VSYNC wait <b id='s-vsync'>0</b> ms</span>\n"
"      <span><i class='read'></i>Sensor readout <b id='s-read'>0</b> ms</span>\n"
"      <span><i class='send'></i>Network send <b id='s-send'>0</b> ms</span>\n"
/* The fourth bar exists because the first three did not add up. They summed to
   52 ms while the measured rate was 9.6 fps - a 104 ms period - so half the
   frame was going somewhere unnamed, and the page was picking a bottleneck out
   of the half it could see.

   This is that half: the wait for the peer to acknowledge the frame before the
   next capture may overwrite the buffer lwIP is still pointing at. On the TOE
   it reads zero, because the chip owns the buffer. One number, and it is the
   cost of terminating TCP on the MCU. */
"      <span><i class='drain'></i>ACK wait <b id='s-drain'>0</b> ms</span>\n"
/* Measured live rather than read off a map file. A link-time figure says
   what was reserved; this says what a 1080p stream actually costs on top
   of it, which is the question a visitor asks about a chip this size. */
"      <span><i class='sram'></i>SRAM <b id='s-sram'>-</b></span>\n"
"    </div>\n"
"    <p class='hint' id='s-hint'>Start streaming to measure.</p>\n"
"  </section>\n"
"  <section class='card tight wide' id='sec-sweep'>\n"
"    <div class='sweep'>\n"
"      <div>\n"
"        <label for='clk'>CLK_DIV <b id='v-clk'>2</b></label>\n"
"        <input type='range' id='clk' min='1' max='16' value='2'>\n"
"      </div>\n"
"      <div>\n"
"        <label for='pll'>PLL_DIV <b id='v-pll'>1</b></label>\n"
"        <input type='range' id='pll' min='1' max='16' value='1'>\n"
"      </div>\n"
"      <button id='apply' class='ghost'>Apply</button>\n"
"      <button id='reset' class='ghost'>Default</button>\n"
"      <button id='recover' class='ghost'>Recover</button>\n"
"    </div>\n"
"  </section>\n"
/* Access and accounts: one card per role, each saying what the role may do.
   Password fields start empty and read-only until focused, so the browser's
   password manager does not pre-fill the admin's own password into them. */
"  <section class='card wide' id='sec-acct' hidden>\n"
"    <h2>Access &amp; accounts</h2>\n"
"    <div class='acards'>\n"
"      <div class='acard'>\n"
"        <h3>Administrator</h3>\n"
"        <p class='desc'>Changes camera settings, saves them, and manages accounts.</p>\n"
"        <div class='id'>User name: <b id='ac-aname'>-</b> (set at setup, fixed)</div>\n"
"        <label for='ac-old'>Current password</label>"
"<input id='ac-old' type='password' autocomplete='new-password' readonly"
" onfocus=\"this.removeAttribute('readonly')\">\n"
"        <label for='ac-new'>New password (at least 10 characters)</label>"
"<input id='ac-new' type='password' autocomplete='new-password' readonly"
" onfocus=\"this.removeAttribute('readonly')\">\n"
"        <div class='row2'><button id='ac-admin' class='primary'>Change password</button></div>\n"
"      </div>\n"
"      <div class='acard'>\n"
"        <h3>Viewer <span class='hint'>(optional)</span></h3>\n"
"        <p class='desc'>Watches the stream and the status only - cannot change anything.</p>\n"
"        <div class='id'>Now: <b id='ac-vstate'>not set</b></div>\n"
"        <label for='ac-vuser'>User name</label>"
"<input id='ac-vuser' autocomplete='off' readonly onfocus=\"this.removeAttribute('readonly')\">\n"
"        <label for='ac-vpass'>Password (at least 10 characters)</label>"
"<input id='ac-vpass' type='password' autocomplete='new-password' readonly"
" onfocus=\"this.removeAttribute('readonly')\">\n"
"        <div class='row2'><button id='ac-viewer' class='primary'>Set viewer</button>"
"<button id='ac-vtoggle' class='warn' hidden>Turn off</button></div>\n"
"      </div>\n"
"    </div>\n"
"    <p id='ac-msg' class='hint'></p>\n"
"  </section>\n"
"  <section class='card wide' id='sec-ctrls'>\n"
"    <h2>Sensor controls</h2>\n"
"    <div id='ctrls' class='ctrls'>loading...</div>\n"
"    <p id='ctrl-msg' class='hint'></p>\n"
"    <div class='saverow'><button id='save' class='primary'>Save as default</button>"
"<span id='saved' class='hint'></span></div>\n"
"    <p id='ctrl-note' class='hint' hidden>Shown is the last value set since the camera "
"started or was reset, not a value read back from it; - means not set since then. "
"* provisional: safe for capture, but what each step means is not confirmed. "
"The camera module may keep its settings across a power cycle.</p>\n"
"  </section>\n"
"</main>\n"
"<footer>Served from the device</footer>\n"
"<script>\n"
"var $=function(id){return document.getElementById(id)};\n"
/* Pixel counts are what the device speaks; mode names are what a visitor
   reads. The keys are exactly the strings /api/status returns, so an unknown
   value falls through to the raw string rather than showing nothing. */
"var RESNAME={'320x240':'QVGA','640x480':'VGA','800x600':'SVGA',\n"
"  '1024x768':'XGA','1280x720':'HD','1280x1024':'SXGA','1600x1200':'UXGA',\n"
"  '1920x1080':'FHD'};\n"
/* Both selects offer only what the sensor accepts: the device lists its sizes
   in res_list. Rebuilt only when the list changes, so an open drop-down is
   not torn down by the next poll. */
"var resKey='';\n"
"function fillRes(list){\n"
"  var k=list.join(',');if(k===resKey){return}resKey=k;\n"
"  ['res','resbar'].forEach(function(id){\n"
"    var el=$(id);el.innerHTML='';\n"
"    list.forEach(function(r){\n"
"      var o=document.createElement('option');o.value=r;\n"
"      o.textContent=(RESNAME[r]?RESNAME[r]+' ':'')+r.replace('x',' x ');\n"
"      el.appendChild(o)});\n"
"  });\n"
"}\n"
"$('addr').textContent=location.host;\n"
"var css=getComputedStyle(document.documentElement);\n"
"var C=function(n){return css.getPropertyValue(n).trim()};\n"
/* stalled counts polls where the device says it is streaming and its frame
   counter has not moved; misses counts polls that did not arrive at all. */
"var last={},lastFrames=-1,stalled=0,misses=0,wasGone=false,opened=false;\n"
"var lastRes=null;   // last resolution the device reported\n"
/* How long the frame counter may sit still before the page calls it a
   stall, and how long it must wait before doing anything about it again.

   Both were far too eager in the first version. Three seconds is less than
   a 720p frame takes to clear a software stack, and the response - reopen
   the stream - tears down the connection the frame was going out on. The
   recovery was manufacturing the fault it was detecting: every restart
   threw away a frame in flight, so the counter never moved, so it restarted
   again. On a slow link that loop never exits. */
"var STALL_POLLS=8,RESTART_COOLDOWN=15000,lastRestart=0;\n"

/* ---- Charts: one sample per status poll, eased so the line glides ----
   Both axes are fixed - 0-30 fps and 0-10 Mbps - so the two boards can be read
   against each other at a glance. An auto-scaling axis would rescale itself per
   board and make the side-by-side comparison meaningless, which matters more
   here than anywhere else on the page: the load demo is exactly a comparison of
   how far two lines fall. */
"var MAXP=60;\n"
"function mkChart(id,rgb,fixed,div){\n"
"  var c={cv:$(id),rgb:rgb,fixed:fixed,div:div,hist:[],shown:0,target:0};\n"
"  c.cx=c.cv.getContext('2d');\n"
"  return c;\n"
"}\n"
"var chF=mkChart('c-fps','0,184,212',30,3);\n"
"var chB=mkChart('c-bw','142,123,239',10,5);\n"
"var charts=[chF,chB];\n"
"function fit(){\n"
"  var d=window.devicePixelRatio||1;\n"
"  charts.forEach(function(c){\n"
"    var r=c.cv.getBoundingClientRect();\n"
"    c.cv.width=Math.max(1,r.width*d);c.cv.height=Math.max(1,r.height*d);\n"
"    c.cx.setTransform(d,0,0,d,0,0);\n"
"  });\n"
"}\n"
"window.addEventListener('resize',fit);fit();\n"
"function push(c,v){\n"
"  c.target=v;c.hist.push(v);if(c.hist.length>MAXP){c.hist.shift()}\n"
"}\n"
"function draw(c){\n"
"  var cx=c.cx,r=c.cv.getBoundingClientRect(),W=r.width,H=r.height;\n"
"  var L=34,R=W-6,T=10,B=H-14,n=c.hist.length;\n"
"  cx.clearRect(0,0,W,H);\n"
"  var mx=c.fixed,dv=c.div;\n"
"  cx.strokeStyle=C('--line');cx.fillStyle=C('--muted');\n"
"  cx.lineWidth=1;cx.font='10px Segoe UI,sans-serif';\n"
"  for(var g=0;g<=dv;g++){\n"
"    var y=T+(B-T)*g/dv,lv=mx*(1-g/dv);\n"
"    cx.beginPath();cx.moveTo(L,y);cx.lineTo(R,y);cx.stroke();\n"
"    cx.fillText(String(Math.round(lv)),4,y+3);\n"
"  }\n"
"  if(n<2){return}\n"
"  var step=(R-L)/(MAXP-1);\n"
"  var X=function(k){return R-(n-1-k)*step};\n"
"  var Y=function(v){return B-(B-T)*Math.min(v,mx)/mx};\n"
"  var path=function(){\n"
"    cx.beginPath();cx.moveTo(X(0),Y(c.hist[0]));\n"
"    for(var k=0;k<n-1;k++){\n"
"      var xc=(X(k)+X(k+1))/2,yc=(Y(c.hist[k])+Y(c.hist[k+1]))/2;\n"
"      cx.quadraticCurveTo(X(k),Y(c.hist[k]),xc,yc);\n"
"    }\n"
"    cx.lineTo(X(n-1),Y(c.hist[n-1]));\n"
"  };\n"
"  var gr=cx.createLinearGradient(0,T,0,B);\n"
"  gr.addColorStop(0,'rgba('+c.rgb+',.34)');\n"
"  gr.addColorStop(1,'rgba('+c.rgb+',0)');\n"
"  path();cx.lineTo(X(n-1),B);cx.lineTo(X(0),B);cx.closePath();\n"
"  cx.fillStyle=gr;cx.fill();\n"
"  path();cx.strokeStyle='rgb('+c.rgb+')';cx.lineWidth=2.5;\n"
"  cx.lineJoin='round';cx.stroke();\n"
"  cx.beginPath();cx.arc(X(n-1),Y(c.hist[n-1]),4,0,6.2832);\n"
"  cx.fillStyle='rgb('+c.rgb+')';cx.fill();\n"
"}\n"
"function tick(){\n"
"  charts.forEach(function(c){\n"
"    c.shown+=(c.target-c.shown)*0.12;\n"
"    if(c.hist.length){c.hist[c.hist.length-1]=c.shown}\n"
"    if(c.cv.offsetParent){draw(c)}\n"
"  });\n"
"  $('s-fps').textContent=chF.shown.toFixed(1);\n"
"  $('s-bw').textContent=chB.shown.toFixed(1);\n"
"  requestAnimationFrame(tick);\n"
"}\n"
"requestAnimationFrame(tick);\n"

/* ---- The three states ----
   A booth visitor sees a picture that is not moving and concludes the product
   is broken. That conclusion is right in exactly one of these cases, so the
   page has to say which one it is in, in words, before anyone asks. */
"function banner(kind,text){\n"
"  var b=$('banner');\n"
"  if(!kind){b.className='';b.textContent='';return}\n"
"  b.className='show '+kind;b.innerHTML=text;\n"
"}\n"
/* No reply at all. The cable is out, the board is rebooting, or the switch
   dropped us. Values on screen are stale and are labelled as such. */
"function offline(e){\n"
"  if(e&&e.auth){return}\n"
"  misses++;\n"
"  if(misses<3){return}\n"
"  wasGone=true;\n"
"  $('dot').className='dot gone';\n"
"  $('s-state').textContent='NO REPLY';\n"
"  banner('gone','No reply from <b>'+location.host+'</b> &mdash; '\n"
"    +'the numbers shown are the last ones it reported.');\n"
"}\n"

/* ---- Every request gets a deadline ----
   Pulling the cable at the DEVICE end does not break anything on this machine:
   the browser's TCP connection to it simply stops being answered, and fetch()
   neither resolves nor rejects while the stack retransmits. That can run for
   tens of seconds. The whole link-down path hangs off .catch(), so with no
   deadline the page never notices the device is gone - it just sits on a still
   picture looking healthy, which is the exact failure this page exists to
   avoid.

   AbortController turns silence into a rejection. Everything the page fetches
   goes through here. */
/* 6 s, not 2.5. On the lwIP image a status response shares the send path
   with a JPEG frame and legitimately takes over a second; 2.5 s turned
   ordinary slowness into "the device is gone", and what the page did about
   that made it true. */
"var POLL_TIMEOUT=6000;\n"
"function deadline(ms){\n"
"  var ac=(typeof AbortController!=='undefined')?new AbortController():null;\n"
"  var d={opt:{cache:'no-store'},done:function(){clearTimeout(d.t)}};\n"
"  if(ac){d.opt.signal=ac.signal}\n"
"  d.t=setTimeout(function(){if(ac){ac.abort()}},ms||POLL_TIMEOUT);\n"
"  return d;\n"
"}\n"

/* ---- Status ----
   409 means a control was clamped. The body is still the status document, so
   render it either way - the panel then snaps back to what the hardware
   actually has, which is the honest answer - but say so, because a slider
   quietly returning to its old position reads as a broken page. */
/* The deadline has to cover the BODY, not just the headers.

   Clearing it when the response line arrived was wrong, and lwIP is where it
   showed: that side sends the header and then drains the body across several
   main-loop passes, so a response can begin and never finish. With the timer
   already cleared, r.json() then waited forever - and because poll() holds a
   single-flight lock, one such response stopped every later poll as well. The
   page sat amber and never came back.

   So: clear it after json() resolves, or on rejection. Never in between. */
/* ---- Login (Phase 3b) ----
   The session is an HttpOnly cookie the page never sees. What the page keeps
   is the CSRF token from /api/auth, sent back on every POST. A 401 anywhere
   means the session is gone: ask /api/auth what to show. */
"var AUTH={role:'none'},CSRF='',ctrlsLoaded=false,toastT=null;\n"
"function toast(text,kind){\n"
"  var t=$('toast');t.textContent=text;\n"
"  t.className=kind===true?'ok':kind==='bad'?'bad':'';t.hidden=false;\n"
"  clearTimeout(toastT);toastT=setTimeout(function(){t.hidden=true},kind===false?6000:3500);\n"
"}\n"
"function withCsrf(o){o.headers=o.headers||{};if(CSRF){o.headers['X-CSRF-Token']=CSRF}return o}\n"
"function enc(o){var a=[];for(var k in o){a.push(encodeURIComponent(k)+'='+encodeURIComponent(o[k]))}\n"
"  return a.join('&')}\n"
"function postForm(url,body){\n"
"  var d=deadline(8000);d.opt.method='POST';\n"
"  d.opt.headers={'Content-Type':'application/x-www-form-urlencoded'};withCsrf(d.opt);\n"
"  d.opt.body=body;\n"
"  return fetch(url,d.opt).then(function(r){return r.text().then(function(t){\n"
"    d.done();var j=null;try{j=JSON.parse(t)}catch(e){}\n"
"    if(r.status===401){authRefresh()}\n"
"    return {ok:r.ok,status:r.status,j:j,t:t}})},function(e){d.done();throw e});\n"
"}\n"
"function errText(x){return (x.j&&x.j.error)?x.j.error:('HTTP '+x.status)}\n"
"function applyAuth(a){\n"
"  AUTH=a;CSRF=a.csrf||'';\n"
"  var on=(a.role==='admin'||a.role==='viewer');\n"
"  $('who').hidden=!on;\n"
"  if(on){$('who-name').textContent=a.user+' ('+a.role+')'}\n"
"  document.body.classList.toggle('is-viewer',a.role==='viewer');\n"
"  $('sec-acct').hidden=(a.role!=='admin');\n"
"  if(a.role==='admin'){\n"
"    $('ac-aname').textContent=a.user;\n"
"    var v=a.viewer||{};\n"
"    $('ac-vstate').textContent=v.present?(v.user+(v.enabled?' - on':' - off')):'not set';\n"
"    if(v.present&&document.activeElement!==$('ac-vuser')){$('ac-vuser').value=v.user}\n"
"    $('ac-viewer').textContent=v.present?'Update viewer':'Set viewer';\n"
"    $('ac-vtoggle').hidden=!v.present;\n"
"    $('ac-vtoggle').textContent=v.enabled?'Turn off':'Turn on';\n"
"  }\n"
"  if(on&&!ctrlsLoaded){loadCtrls()}\n"
"  showAuth(on?null:a);\n"
"}\n"
"function showAuth(a){\n"
"  if(!a){$('authbox').hidden=true;return}\n"
/* Logged out (or never in): stop the stream and wipe the last frame, so the
   picture does not stay on the page after the session that showed it. */
"  if(typeof sCtl!=='undefined'&&sCtl){try{sCtl.abort()}catch(e){}sCtl=null}\n"
/* ...and forget that a stream was ever opened, so the first status after the
   next login opens it straight away. Without this the page waited for its
   stall detector (8 polls, ~16 s) after a reboot ended the session. */
"  if(typeof opened!=='undefined'){opened=false}\n"
"  var cv=$('view');if(cv&&cv.getContext){cv.getContext('2d').clearRect(0,0,cv.width,cv.height)}\n"
"  var setup=!a.provisioned;\n"
"  $('authbox').hidden=false;\n"
"  $('auth-title').textContent=setup?'Set up this camera':'Log in';\n"
"  $('a-pass2-w').hidden=!setup;\n"
"  $('a-go').textContent=setup?'Create admin':'Log in';\n"
"  $('a-go').disabled=setup&&!a.setup_open;\n"
"  $('auth-text').textContent=!setup?'':a.setup_open\n"
"    ?'Setup mode is open. Choose the admin user name and a password of at least 10 characters.'\n"
"    :'No admin account yet. Open setup mode on the device (type setup on its USB console), then reload.';\n"
"}\n"
"function authRefresh(){\n"
"  return fetch('/api/auth',{cache:'no-store'}).then(function(r){\n"
"    if(!r.ok){throw new Error('no login on this build')}return r.json()}).then(applyAuth)\n"
"    .catch(function(){});\n"
"}\n"
"$('authform').onsubmit=function(ev){\n"
"  ev.preventDefault();\n"
"  var setup=!AUTH.provisioned,u=$('a-user').value,p=$('a-pass').value;\n"
"  if(setup&&p!==$('a-pass2').value){$('auth-msg').textContent='The passwords differ';return}\n"
"  $('auth-msg').textContent='...';\n"
"  postForm(setup?'/api/setup':'/api/login',enc({user:u,pass:p})).then(function(x){\n"
"    $('a-pass').value='';$('a-pass2').value='';\n"
"    if(x.ok&&x.j){$('auth-msg').textContent='';applyAuth(x.j);poll();return}\n"
"    $('auth-msg').textContent=errText(x);\n"
"  }).catch(function(){$('auth-msg').textContent='No reply'});\n"
"};\n"
"$('logout').onclick=function(){\n"
"  postForm('/api/logout','').then(function(){location.reload()},function(){location.reload()});\n"
"};\n"
"function acct(o){\n"
"  $('ac-msg').textContent='...';$('ac-msg').className='hint';\n"
"  postForm('/api/account',enc(o)).then(function(x){\n"
"    if(x.ok&&x.j){applyAuth(x.j);$('ac-msg').textContent='Saved';return}\n"
"    $('ac-msg').textContent=errText(x);$('ac-msg').className='hint bad';\n"
"  }).catch(function(){$('ac-msg').textContent='No reply';$('ac-msg').className='hint bad'});\n"
"}\n"
"$('ac-admin').onclick=function(){\n"
"  acct({role:'admin',old:$('ac-old').value,pass:$('ac-new').value});\n"
"  $('ac-old').value='';$('ac-new').value='';\n"
"};\n"
"$('ac-viewer').onclick=function(){\n"
"  acct({role:'viewer',user:$('ac-vuser').value,pass:$('ac-vpass').value});$('ac-vpass').value='';\n"
"};\n"
"$('ac-vtoggle').onclick=function(){\n"
"  acct({role:'viewer',enabled:(AUTH.viewer&&AUTH.viewer.enabled)?'0':'1'});\n"
"};\n"
/* State changes are POST with the CSRF token; the status poll stays GET. A
   refusal that is about the login is not "the device is gone". */
"function call(p){\n"
"  var d=deadline(POLL_TIMEOUT);\n"
"  if(/^\\/api\\/(start|stop|reset|res)\\b/.test(p)){d.opt.method='POST';withCsrf(d.opt)}\n"
"  return fetch(p,d.opt).then(function(r){\n"
"    if(r.status===401||r.status===403){\n"
"      d.done();\n"
"      return r.json().catch(function(){return {}}).then(function(j){\n"
"        if(r.status===401||j.error==='setup required'){authRefresh()}\n"
"        else{$('s-hint').textContent='Refused: '+(j.error||r.status)}\n"
"        var e=new Error('auth');e.auth=true;throw e;\n"
"      });\n"
"    }\n"
"    var refused=(r.status===409);\n"
"    return r.json().then(function(s){\n"
"      d.done();\n"
/* A throw inside render() used to reach the outer .catch, which is the
   link-down path - so a bug in this page presented as "the device is not
   answering", pointing every investigation at the firmware. The response
   arrived; only the drawing failed. Say which. */
"      try{render(s)}catch(err){\n"
"        $('dot').className='dot stall';\n"
"        $('s-state').textContent='PAGE ERROR';\n"
"        banner('stall','The device replied; this page failed to draw it: <b>'\n"
"          +(err&&err.message?err.message:err)+'</b>');\n"
"        if(window.console){console.error(err)}\n"
"      }\n"
"      if(refused){$('s-hint').innerHTML='The sensor <b>refused</b> that '\n"
"        +'setting; showing what it actually has.'}\n"
"      return s;\n"
"    });\n"
"  }).catch(function(e){d.done();throw e});\n"
"}\n"
"function render(s){\n"
/* A poll getting through after the link was down is the earliest moment the
   view can come back. Waiting for the stall counter would spend three more
   seconds staring at a frozen frame for a link already known to be up. */
"  if(wasGone){wasGone=false;banner(null);setTimeout(function(){restartStream(false,'back')},300)}\n"
/* First reply that says it is streaming: open the view. Doing it here rather
   than in the markup keeps the stream connection out of the page transfer, and
   means the connection is only ever opened against a device that has frames to
   put on it. force=true - this is the one opening that must not be held back
   by the reopen cooldown. */
/* Only counts as opened when the login card is gone - restartStream() does
   nothing while it shows, and a flag set then would leave the stream to the
   stall detector after the login. */
"  if(!opened&&s.streaming&&$('authbox').hidden){opened=true;restartStream(true,'open')}\n"
"  misses=0;\n"
"  $('s-res').textContent=s.res;\n"
"  $('s-frames').textContent=s.frames;\n"
"  $('s-drop').textContent=s.dropped;\n"
"  $('start').disabled=s.streaming;\n"
"  $('stop').disabled=!s.streaming;\n"
"  if(s.res_list){fillRes(s.res_list)}\n"
"  if($('res').value!==s.res){$('res').value=s.res}\n"
"  if(document.activeElement!==$('resbar')&&$('resbar').value!==s.res){$('resbar').value=s.res}\n"
/* Clear the traces on any resolution change, including one this page did
   not ask for.

   The averages are the reason. min/avg/max are computed over the whole
   history buffer, so HD samples left in it after a switch to FHD drag the
   average toward a frame size that is no longer being sent - the page then
   reports a number the device never achieved in either mode. The step in
   the trace reads as the stack faltering when it is only a different
   amount of data per frame.

   Keyed off the reported value, not off the click, so a change made from
   the other control - or from a second browser - clears the history too. */
"  if(lastRes!==null&&lastRes!==s.res){\n"
"    charts.forEach(function(c){c.hist=[];c.shown=0;c.target=0});\n"
"    $('s-min').textContent='-';$('s-avg').textContent='-';\n"
"    $('s-max').textContent='-';$('s-bwmax').textContent='-';\n"
"  }\n"
"  lastRes=s.res;\n"
"  $('resnow').textContent=RESNAME[s.res]||s.res;\n"

"  $('s-kb').textContent=s.kb;\n"
"  $('s-vsync').textContent=s.vsync_ms;\n"
"  $('s-read').textContent=s.read_ms;\n"
"  $('s-send').textContent=s.send_ms;\n"
"  $('s-drain').textContent=(s.drain_ms||0);\n"
"  if(s.sram_total){\n"
"    var kb=Math.round(s.sram_used/1024),tot=Math.round(s.sram_total/1024);\n"
"    $('s-sram').textContent=kb+' / '+tot+' KB ('+\n"
"      (s.sram_used*100/s.sram_total).toFixed(1)+'%)';\n"
"  }\n"
"  var loadMbps=(s.load_kbps||0)/1000;\n"
"  $('s-load').textContent=loadMbps.toFixed(1);\n"
"  $('s-loadkb').textContent=loadKB?(loadKB+' KB/req'):'off';\n"
"  push(chF,s.fps);\n"
/* What the stream costs the link: frames per second times frame size. The load
   is deliberately NOT added into this line - the exhibit is watching the stream
   line fall while the load runs beside it, and summing them would hide exactly
   that. */
"  push(chB,s.fps*s.kb*8/1000);\n"
"  $('h-fps').textContent=s.fps.toFixed(1);\n"
"  $('h-bw').textContent=(s.fps*s.kb*8/1000).toFixed(1);\n"
"  var mn=1e9,mx=0,sum=0,bmx=0,i,v;\n"
"  for(i=0;i<chF.hist.length;i++){v=chF.hist[i];\n"
"    if(v<mn){mn=v}if(v>mx){mx=v}sum+=v}\n"
"  for(i=0;i<chB.hist.length;i++){if(chB.hist[i]>bmx){bmx=chB.hist[i]}}\n"
"  if(chF.hist.length){\n"
"    $('s-min').textContent=mn.toFixed(1);\n"
"    $('s-max').textContent=mx.toFixed(1);\n"
"    $('s-avg').textContent=(sum/chF.hist.length).toFixed(1);\n"
"    $('s-bwmax').textContent=bmx.toFixed(1)+' Mbps';\n"
"  }\n"
"  $('s-hint').innerHTML=hint(s);\n"
"  if(s.stack){\n"
"    var toe=(s.stack.indexOf('TOE')===0);\n"
/* The badge is the one thing a visitor is meant to read from across the
   room, so it carries the brand on the TOE side: "TOE" alone means
   nothing to someone who has not been told what it stands for, and the
   point of the exhibit is that the WIZnet chip is doing the work. lwIP is
   left as a bare name - it is the thing being compared against, not the
   thing being sold. */
"    $('stack').textContent=toe?'WIZnet TOE':'lwIP';\n"
"    $('what').textContent=toe\n"
"      ?'Hardwired TCP/IP - the WIZnet chip terminates TCP'\n"
"      :'Software TCP/IP - lwIP terminates TCP on the MCU';\n"
"    document.documentElement.style.setProperty('--stack',\n"
/* Red for the TOE, grey for lwIP - not a second colour. A near-black
   badge still reads as an emphasis; grey reads as the plain alternative,
   which is the comparison the exhibit is making. #6f7a86 is dark enough
   to keep white text legible on a projector. */
"      toe?'#e01b24':'#6f7a86');\n"
"    document.title=s.stack+' camera';\n"
"  }\n"
"  if(document.activeElement!==$('clk')&&document.activeElement!==$('pll')){\n"
"    $('clk').value=s.clk_div;$('v-clk').textContent=s.clk_div;\n"
"    $('pll').value=s.pll_div;$('v-pll').textContent=s.pll_div;\n"
"  }\n"
"  if(s.ctrl){renderCtrls(s.ctrl)}\n"
"  savedLine(s);\n"

/* ---- Which of the three states are we in ----
   The device tells us enough to decide without guessing: it reports whether it
   was asked to stream, whether its frame counter is moving, and how many load
   bytes it actually served. Order matters - a stalled stream under load is
   still a stalled stream, so that test comes first. */
"  var moving=(s.frames!==lastFrames);\n"
"  lastFrames=s.frames;\n"
"  if(!s.streaming){\n"
"    stalled=0;$('dot').className='dot';\n"
"    $('s-state').textContent='STOPPED';banner(null);\n"
/* Both conditions, not just the counter. fps is measured by the device over
   its own one-second window, so "counter did not move AND the device says
   its rate is zero" is the unambiguous case. The counter alone goes still
   for a second whenever a frame is simply large. */
"  }else if(!moving&&s.fps===0){\n"
"    stalled++;\n"
"    if(stalled>=STALL_POLLS){\n"
"      $('dot').className='dot stall';\n"
"      $('s-state').textContent='NO FRAMES';\n"
"      banner('stall','No frames for '+STALL_POLLS+' s. The device '\n"
"        +'is answering, so this is the camera or a frame too large for '\n"
"        +'this link to carry. Try a smaller resolution, then <b>Recover</b>.');\n"
/* Reopening the stream throws away whatever was in flight, so it is a last
   resort on a timer rather than something to do every few seconds. */
"      restartStream(false,'stall');\n"
"      stalled=STALL_POLLS;\n"
"    }\n"
"  }else{\n"
"    stalled=0;\n"
"    if(s.load_kbps>0){\n"
"      $('dot').className='dot loaded';\n"
"      $('s-state').textContent='STREAMING + LOAD';\n"
"      banner('loaded','Link under load: '+loadMbps.toFixed(1)+' Mbps of filler '\n"
"        +'alongside the video. A lower frame rate here is the load, '\n"
"        +'not a fault.');\n"
"    }else{\n"
"      $('dot').className='dot live';\n"
"      $('s-state').textContent='STREAMING';banner(null);\n"
"    }\n"
"  }\n"
/* Frames bigger than the buffer are dropped by the device; at a size where
   every frame is, the picture just stops. Say why. */
/* Only when no frame got through since the last poll: at a size where some
   frames fit, a banner every other poll was noise. */
"  if(s.streaming&&last.too_big!==undefined&&s.too_big>last.too_big&&s.frames===last.frames){\n"
"    banner('stall','Frames at '+(RESNAME[s.res]||s.res)+' are larger than the device\\'s '\n"
"      +'96 KB frame buffer and are being dropped. Choose a smaller resolution.');\n"
"  }\n"
"  last=s;\n"
"}\n"
"function hint(s){\n"
"  if(!s.streaming){return 'Start streaming to measure.'}\n"
"  if(s.load_kbps>0){\n"
"    return 'Under load the bar that grows should be <b>network send</b>. '+\n"
"      'VSYNC and readout are the sensor and do not know the link is busy - '+\n"
"      'if they move too, the load is stealing MCU time, which is what a '+\n"
"      'software stack does and a hardwired one does not.'}\n"
/* Checked first, because it was the invisible one and on the software stack it
   is usually the largest. */
"  var dr=s.drain_ms||0;\n"
"  if(dr>=s.vsync_ms&&dr>=s.read_ms&&dr>=s.send_ms){\n"
"    return 'Bottleneck: <b>ACK wait</b>. The frame is on the wire, but the '+\n"
"      'next one cannot be captured until the peer acknowledges this one - '+\n"
"      'lwIP holds a pointer into the capture buffer rather than a copy. '+\n"
"      'A hardwired stack does not pay this at all.'}\n"
"  if(s.vsync_ms>=s.read_ms&&s.vsync_ms>=s.send_ms){\n"
"    return 'Bottleneck: <b>VSYNC wait</b>. The device finishes early and idles'+\n"
"      ' until the sensor starts the next frame, so the rate is quantised by'+\n"
"      ' the sensor mode.'}\n"
"  if(s.read_ms>=s.send_ms){\n"
"    return 'Bottleneck: <b>sensor readout</b>. Pixel data takes the longest to'+\n"
"      ' pull out; the sensor clock below is the lever.'}\n"
"  return 'Bottleneck: <b>network send</b>. Pushing the JPEG out takes the'+\n"
"    ' longest - this is where a hardwired stack and a software stack differ.';\n"
"}\n"

/* ---- Sensor controls, built from what the device reports ----
   The device owns the list: names, labels, groups and ranges all arrive from
   /api/controls, so a control added in cam_controls.c appears here on its own.
   A range of 0..1 is a checkbox and anything wider is a slider. */
"var ctrlEls={},lastCtrl={};\n"
"function buildCtrls(list){\n"
"  var box=$('ctrls');\n"
/* An empty list is a driver that offers no controls (OV2640 until its new
   board is verified), not a failure. Say which it is. */
"  if(!list.length){box.textContent='This camera offers no controls in this build.';return}\n"
"  box.innerHTML='';\n"
"  var d=document.createElement('div');d.className='grp';\n"
"  list.forEach(function(c){d.appendChild(ctrlRow(c))});\n"
"  box.appendChild(d);\n"
"  $('ctrl-note').hidden=false;\n"
"  renderCtrls(lastCtrl,true);\n"
"}\n"
/* enum -> select, bool -> checkbox, int -> slider. Nothing is pre-filled with a
   guessed default: until a value has been set, the device has not said what
   the sensor holds, so the control shows '-' (indeterminate for a checkbox). */
"function ctrlRow(c){\n"
"  var row=document.createElement('div');row.className='row';\n"
"  var nm=document.createElement('span');nm.className='nm';\n"
"  nm.textContent=c.label+(c.provisional?' *':'');row.appendChild(nm);\n"
"  var el,vl=document.createElement('span');vl.className='vl';vl.textContent='-';\n"
"  if(c.type==='enum'){\n"
"    el=document.createElement('select');\n"
"    var o=document.createElement('option');o.value='';o.textContent='-';el.appendChild(o);\n"
"    (c.options||[]).forEach(function(t,i){\n"
"      var p=document.createElement('option');p.value=String(i);p.textContent=t;el.appendChild(p)});\n"
"    el.onchange=function(){if(el.value!==''){sendCtrl(c.name,el.value)}};\n"
"  }else if(c.type==='bool'){\n"
"    el=document.createElement('input');el.type='checkbox';\n"
"    el.onchange=function(){sendCtrl(c.name,el.checked?1:0)};\n"
"  }else{\n"
"    el=document.createElement('input');el.type='range';el.min=c.min;el.max=c.max;\n"
"    el.value=(c.min<=0&&c.max>=0)?0:c.min;\n"
/* Follow the thumb live for the label, but only send on release: a drag across
   the range would otherwise fire one request per notch. */
"    el.oninput=function(){vl.textContent=el.value};\n"
"    el.onchange=function(){sendCtrl(c.name,el.value)};\n"
"  }\n"
"  row.appendChild(el);row.appendChild(vl);\n"
"  ctrlEls[c.name]={el:el,vl:vl,c:c};\n"
"  return row;\n"
"}\n"
"function ctrlMsg(t,bad){var m=$('ctrl-msg');m.textContent=t;m.className=bad?'hint bad':'hint'}\n"
/* POST, form-encoded. A refusal is shown with the device's own reason and
   the control snaps back to the last value the device reported as written -
   never left showing a value that was not applied. */
"function savedLine(st){\n"
"  var x=st&&st.settings;if(!x){return}\n"
"  $('saved').textContent=x.res?('Saved: '+x.res+(x.ctrl?', '+x.ctrl:'')+' ('+x.note+')')\n"
"    :('Nothing saved ('+x.note+')');\n"
"}\n"
/* Saving writes flash, so it is a button of its own and never a side effect
   of moving a control. */
"$('save').onclick=function(){\n"
"  var d=deadline(8000);d.opt.method='POST';withCsrf(d.opt);\n"
"  ctrlMsg('Saving ...',false);\n"
"  fetch('/api/settings/save',d.opt).then(function(r){\n"
"    return r.text().then(function(t){\n"
"      d.done();var j=null;try{j=JSON.parse(t)}catch(e){}\n"
"      if(r.ok&&j){ctrlMsg('Saved; used at the next boot',false);render(j);\n"
"        toast('\\u2713 Saved as default: '+(RESNAME[j.res]||j.res)+\n"
"          ((j.settings&&j.settings.ctrl)?', '+j.settings.ctrl:''),true);return}\n"
"      toast('Save failed',\"bad\");\n"
"      ctrlMsg(r.status+': '+((j&&j.error)?j.error:t),true);\n"
"    });\n"
"  }).catch(function(){d.done();ctrlMsg('No reply: may or may not have been saved',true)});\n"
"};\n"
"function sendCtrl(name,value){\n"
"  var d=deadline(5000);\n"
"  d.opt.method='POST';\n"
"  d.opt.headers={'Content-Type':'application/x-www-form-urlencoded'};withCsrf(d.opt);\n"
"  d.opt.body=encodeURIComponent(name)+'='+encodeURIComponent(value);\n"
"  ctrlMsg('Setting '+name+' ...',false);\n"
"  fetch('/api/cam',d.opt).then(function(r){\n"
"    return r.text().then(function(t){\n"
"      d.done();\n"
"      var j=null;try{j=JSON.parse(t)}catch(e){}\n"
"      if(r.ok&&j){ctrlMsg(name+' = '+value+' written',false);render(j);return}\n"
"      ctrlMsg(r.status+': '+((j&&j.error)?j.error:t)+((j&&j.control)?' ('+j.control+')':''),true);\n"
"      renderCtrls(lastCtrl,true);\n"
"    });\n"
"  }).catch(function(){\n"
"    d.done();\n"
"    ctrlMsg('No reply: the change may or may not have been applied',true);\n"
"    renderCtrls(lastCtrl,true);\n"
"  });\n"
"}\n"
"function renderCtrls(values,force){\n"
"  lastCtrl=values||{};\n"
"  for(var k in ctrlEls){\n"
"    var e=ctrlEls[k],has=Object.prototype.hasOwnProperty.call(lastCtrl,k),v=lastCtrl[k];\n"
/* Leave whatever the user is touching alone; the poll would otherwise snap the
   thumb back mid-drag. */
"    if(!force&&document.activeElement===e.el){continue}\n"
"    e.vl.textContent=has?String(v):'-';\n"
/* Two states only: not set since the last reset reads as off, which is what
   the sensor's own initialisation gives. A third "-" state confused more than
   it told (2026-10-26). */
"    if(e.c.type==='bool'){e.el.checked=has&&!!v}\n"
"    else if(e.c.type==='enum'){e.el.value=has?String(v):''}\n"
"    else{e.el.value=has?v:((e.c.min<=0&&e.c.max>=0)?0:e.c.min)}\n"
"  }\n"
"}\n"
/* The simplified exhibition layout hides this panel; #controls in the address
   brings it back without a different build. */
"function ctrlHash(){document.body.classList.toggle('show-ctrls',location.hash==='#controls')}\n"
"ctrlHash();window.addEventListener('hashchange',ctrlHash);\n"
"function loadCtrls(){\n"
"  fetch('/api/controls',{cache:'no-store'}).then(function(r){\n"
"    if(!r.ok){throw new Error(r.status)}return r.json()})\n"
"    .then(function(l){ctrlsLoaded=true;buildCtrls(l)})\n"
"    .catch(function(){$('ctrls').textContent='controls unavailable'});\n"
"}\n"
"loadCtrls();\n"

/* ---- Load generator ----
   One request at a time, the next issued only when the last has landed. Firing
   several in parallel would open several connections and then the demo would be
   measuring how many sockets each stack has rather than what a byte costs it.
   See the note in exhibition_config.h. */
"var loadKB=0,loadRunning=false,lseq=0;\n"
"function loadTick(){\n"
"  if(loadKB<=0){loadRunning=false;return}\n"
"  loadRunning=true;\n"
/* Ten seconds, not the poll deadline: 128 KB over a loaded link legitimately
   takes a while. Cleared after the body, for the reason in call(). */
"  var d=deadline(10000);\n"
"  fetch('/load?kb='+loadKB+'&n='+(++lseq),d.opt)\n"
"    .then(function(r){return r.arrayBuffer()})\n"
"    .then(function(){d.done();setTimeout(loadTick,0)})\n"
/* A failed load request is not evidence the device is gone - under heavy load
   it may simply have had no socket free - so back off and retry rather than
   declaring the link down. */
"    .catch(function(){d.done();setTimeout(loadTick,500)});\n"
"}\n"
"Array.prototype.forEach.call($('seg').children,function(b){\n"
"  b.onclick=function(){\n"
"    Array.prototype.forEach.call($('seg').children,function(o){\n"
"      o.className=''});\n"
"    b.className='on';\n"
"    loadKB=+b.getAttribute('data-kb');\n"
"    $('s-loadkb').textContent=loadKB?(loadKB+' KB/req'):'off';\n"
"    if(loadKB>0&&!loadRunning){loadTick()}\n"
"  };\n"
"});\n"

/* ---- Controls ---- */
/* ---- Reopening the stream is expensive, so it is rate limited ----

   Every reopen is a new TCP connection plus a close on the old one, and the
   old one lingers because the browser has already abandoned it. On the lwIP
   image those linger in a pcb pool of five (MEMP_NUM_TCP_PCB), so a reopen
   once a second exhausts it and the next connection is never accepted -
   which the page reads as the device being gone, and answers by reopening
   the stream. Measured: /api/status answered and closed every time in the
   serial log, while the browser reported ERR_CONNECTION_TIMED_OUT for the
   connections that never got a pcb.

   The recovery was manufacturing the fault. A cable pull is a one-off and
   still gets its immediate reopen; a loop gets one every 12 s, which is not
   enough to starve the pool. Anything a person clicked passes force=true,
   because they are waiting for it and they cannot click once a second. */
/* seq starts from the clock rather than from zero.

   It was `var seq=0`, so the first stream URL of every page load was the same
   string - /stream?1 - and a reload asked for a resource the browser had been
   streaming under that exact name a moment earlier. Chrome does not treat
   multipart/x-mixed-replace like an ordinary response and Cache-Control:
   no-store did not change its mind: reloading gave a live picture or a black
   one, alternating, while the board reported STREAMING at full frame rate into
   a connection the page was not reading.

   The serial log is what gave it away - /stream?1 twice, once per load:

       [req s1] /stream?1        <- first load
       [req s0] /stream?1        <- reload, same URL

   Date.now() makes the first URL of a load different from the last one, which
   is all this needed. */
/* ---- The picture: the page reads the stream itself (Phase 1c) ----

   Until 2026-10-23 the stream went into an <img>. Chrome fires `load` on a
   multipart <img> for the first part only, so the page could not tell a live
   picture from a frozen one: it guessed, tore the stream down every ~12 s
   (board log "206 frames in 12 s, last sent 30 ms ago", page "1,11900"), the
   fresh <img> showed black until its first frame, and now and then it never
   drew at all until the page was reloaded. One corrupt JPEG part also froze
   an <img> for good.

   Now fetch() reads the multipart body, each part is cut out by its
   Content-Length, decoded with createImageBitmap() and drawn on the canvas.
   So the page knows exactly when a frame was drawn; a part that fails to
   decode is counted and skipped instead of freezing the view; and during a
   reconnect the canvas keeps the last frame, so nothing goes black. Decoding
   keeps only the newest pending part: if the browser falls behind it drops
   frames rather than queueing them.

   seq starts from the clock: a reload must not ask for the URL the browser
   was streaming a moment earlier (Chrome treated /stream?1 twice as the same
   resource and alternated live and black, 2026-10). */
"var seq=Date.now(),lastRestart=0,RESTART_COOLDOWN=12000;\n"
"var sCtl=null,sDrawn=0,sBad=0,lastFrameAt=Date.now(),lastErr=0,ERR_COOLDOWN=3000;\n"
"var decoding=false,pendingJpg=null,pendingCtl=null;\n"
/* p= tells the board what the connection being replaced did: frames drawn,
   ms since the last one, parts that failed to decode. Printed next to the
   board's own count in "Stream opened ... page: n,ms,bad". */
"function restartStream(force,why){\n"
"  if(!$('authbox').hidden){return}       /* not logged in: no stream to ask for */\n"
"  var now=Date.now();\n"
"  if(!force&&now-lastRestart<RESTART_COOLDOWN){return}\n"
"  lastRestart=now;\n"
"  var p=sDrawn+','+(now-lastFrameAt)+','+sBad;sDrawn=0;sBad=0;\n"
"  if(sCtl){try{sCtl.abort()}catch(e){}}\n"
"  var ctl=new AbortController();sCtl=ctl;pendingJpg=null;pendingCtl=null;\n"
"  readStream('/stream?'+(++seq)+'&r='+(why||'user')+'&p='+p,ctl);\n"
"}\n"
"function hdrEnd(b){\n"
"  for(var i=0;i+3<b.length;i++){\n"
"    if(b[i]===13&&b[i+1]===10&&b[i+2]===13&&b[i+3]===10){return i}\n"
"  }\n"
"  return -1;\n"
"}\n"
/* Parts are "--boundary CRLF headers CRLF CRLF <Content-Length bytes> CRLF".
   The CRLF after a JPEG becomes the start of the next part's header block,
   which is harmless: only the first CRLFCRLF is looked for. */
"function readStream(url,ctl){\n"
"  fetch(url,{signal:ctl.signal,cache:'no-store'}).then(function(r){\n"
"    if(r.status===409){\n"
"      banner('loaded','Another session is watching this camera; trying again shortly.');\n"
"      var be=new Error('busy');be.busy=true;throw be;\n"
"    }\n"
"    if(r.status===401){authRefresh();var ae=new Error('auth');ae.auth=true;throw ae}\n"
"    if(!r.ok||!r.body){throw new Error('HTTP '+r.status)}\n"
"    var rd=r.body.getReader(),buf=new Uint8Array(0),need=-1,dec=new TextDecoder();\n"
"    function pump(){return rd.read().then(function(x){\n"
"      if(x.done){throw new Error('stream ended')}\n"
"      var nb=new Uint8Array(buf.length+x.value.length);\n"
"      nb.set(buf);nb.set(x.value,buf.length);buf=nb;\n"
"      for(;;){\n"
"        if(need<0){\n"
"          var h=hdrEnd(buf);\n"
"          if(h<0){if(buf.length>4096){throw new Error('no part header')}break}\n"
"          var m=/Content-Length:\\s*(\\d+)/i.exec(dec.decode(buf.subarray(0,h)));\n"
"          if(!m){throw new Error('part without length')}\n"
"          need=parseInt(m[1],10);\n"
"          if(need>4194304){throw new Error('part too large')}\n"
"          buf=buf.subarray(h+4);\n"
"        }\n"
"        if(buf.length<need){break}\n"
"        showFrame(buf.slice(0,need),ctl);\n"
"        buf=buf.subarray(need);need=-1;\n"
"      }\n"
"      return pump();\n"
"    })}\n"
"    return pump();\n"
"  }).catch(function(e){\n"
"    if(ctl!==sCtl||ctl.signal.aborted){return}\n"
"    if(e&&(e.busy||e.auth)){return}         /* the watchdog retries later */\n"
/* An error is not a person: the immediate retry is rate limited (the lwIP
   image once reopened three times in a millisecond and left a pcb in
   TIME_WAIT each time). Inside the cooldown the watchdog below retries. */
"    var n=Date.now();\n"
"    if(n-lastErr<ERR_COOLDOWN){return}\n"
"    lastErr=n;restartStream(true,'err');\n"
"  });\n"
"}\n"
"function showFrame(jpg,ctl){\n"
"  if(decoding){pendingJpg=jpg;pendingCtl=ctl;return}\n"
"  decoding=true;\n"
"  createImageBitmap(new Blob([jpg],{type:'image/jpeg'})).then(function(bm){\n"
"    if(ctl===sCtl){\n"
"      var c=$('view');\n"
"      if(c.width!==bm.width||c.height!==bm.height){c.width=bm.width;c.height=bm.height}\n"
"      c.getContext('2d').drawImage(bm,0,0);\n"
"      sDrawn++;lastFrameAt=Date.now();\n"
"    }\n"
"    if(bm.close){bm.close()}\n"
"  },function(){if(ctl===sCtl){sBad++}}).then(function(){\n"
"    decoding=false;\n"
"    if(pendingJpg){\n"
"      var j=pendingJpg,k=pendingCtl;pendingJpg=null;pendingCtl=null;\n"
"      if(k===sCtl){showFrame(j,k)}\n"
"    }\n"
"  });\n"
"}\n"
/* ---- Noticing that the picture stopped ----

   A frame drawn is now a fact, not a guess, so the check is short: nothing
   drawn for 4 s while the device says it is streaming means reconnect. Not
   while the device is stopped (/api/stop) - no frames is the right answer
   then - and not while the tab is hidden: browsers throttle background tabs,
   and the clock restarts when the tab is shown again (2026-10-22: a hidden
   stream tab looked like a freeze every few seconds). */
"document.addEventListener('visibilitychange',function(){\n"
"  if(!document.hidden){lastFrameAt=Date.now()}\n"
"});\n"
"setInterval(function(){\n"
"  if(document.hidden||!sCtl||last.streaming===false){lastFrameAt=Date.now();return}\n"
"  if(Date.now()-lastFrameAt>4000){restartStream(false,'idle')}\n"
"},1000);\n"
/* Every one of these ends in .catch(offline) for the same reason the poll does:
   a control that cannot reach the device is itself evidence the link is down,
   and an uncaught rejection would leave the page looking healthy while the
   button quietly did nothing. */
"$('start').onclick=function(){call('/api/start').then(function(){\n"
"  setTimeout(function(){restartStream(true)},150)}).catch(offline)};\n"
"$('stop').onclick=function(){call('/api/stop').catch(offline)};\n"
/* One path for changing resolution, whichever control asked for it.

   Neither the lit button nor the cleared history is decided here. Both are
   driven off the next status reply, so they follow what the camera actually
   did - if the device refuses a mode, the page does not throw away a good
   trace or move the highlight for a change that never happened. */
/* Rate limited, because a resolution change is far more expensive on the
   device than it looks from the page.

   Each one tears the stream connection down and opens a new one, and the
   connections the browser had in flight go with it. Measured on the lwIP
   image: one change leaves six or seven pcbs in TIME_WAIT for 2*MSL, two in
   quick succession reach 8/10, and four reach 10/10 - the entire pool, one
   active connection and nine waiting. lwIP then serves the next connection by
   killing something, and nothing guarantees it picks a finished connection
   over the live stream.

   Three seconds is longer than the 2*MSL of 10 s divided by the pool, which
   is the rate the device can actually retire them, but short enough that a
   visitor pressing the other button does not think it is broken. The pool was
   raised to 16 as well; this is the half that stops the burst from happening
   rather than the half that absorbs it. */
/* Inside the cooldown the choice used to be dropped without a word: the
   drop-down showed UXGA while the camera stayed at HD (2026-10-26). Now it is
   applied when the cooldown ends, and the page says so. */
"var RES_COOLDOWN=3000,lastRes_ms=0,resTimer=null;\n"
"function applyRes(v){\n"
"  var wait=RES_COOLDOWN-(Date.now()-lastRes_ms);\n"
"  clearTimeout(resTimer);\n"
"  if(wait>0){\n"
"    toast('Switching to '+(RESNAME[v]||v)+' in '+Math.ceil(wait/1000)+' s ...',false);\n"
"    resTimer=setTimeout(function(){applyRes(v)},wait);\n"
"    return;\n"
"  }\n"
"  lastRes_ms=Date.now();\n"
"  toast('Switching to '+(RESNAME[v]||v)+' ...',false);\n"
"  call('/api/res?v='+v).then(function(){\n"
"    setTimeout(function(){restartStream(true)},400)})\n"
"    .catch(offline);\n"
"}\n"
"$('res').onchange=function(){applyRes(this.value)};\n"
/* blur: while the drop-down keeps focus the poll will not correct it, so a
   refused change would go on showing the size that was not applied. */
"$('resbar').onchange=function(){this.blur();applyRes(this.value)};\n"
"$('clk').oninput=function(){$('v-clk').textContent=this.value};\n"
"$('pll').oninput=function(){$('v-pll').textContent=this.value};\n"
"function applyClk(c,p){\n"
"  $('clk').value=c;$('pll').value=p;\n"
"  $('v-clk').textContent=c;$('v-pll').textContent=p;\n"
"  charts.forEach(function(x){x.hist=[];x.shown=0;x.target=0});\n"
"  return call('/api/clk?div='+c+'&pll='+p)\n"
"    .then(function(){setTimeout(function(){restartStream(true)},400)})\n"
"    .catch(offline);\n"
"}\n"
"$('apply').onclick=function(){applyClk($('clk').value,$('pll').value)};\n"
"$('reset').onclick=function(){applyClk(2,1)};\n"
"$('recover').onclick=function(){\n"
"  $('recover').textContent='...';\n"
"  call('/api/reset').then(function(){\n"
"    $('recover').textContent='Recover';\n"
"    setTimeout(function(){restartStream(true)},400)})\n"
"    .catch(function(){$('recover').textContent='Recover';offline()});\n"
"};\n"
/* One poll in flight at a time. With a 2.5 s deadline and a 1 s interval a
   slow device would otherwise accumulate overlapping requests, and on the
   TOE each one costs a hardware socket. */
"var polling=false,pollAt=0;\n"
"function poll(){\n"
/* One poll in flight at a time - on the TOE each one costs a hardware socket.
   The stale check is the belt to that braces: if a poll ever fails to settle,
   the lock must not be what keeps the page dark. */
"  if(polling&&Date.now()-pollAt<POLL_TIMEOUT*2){return}\n"
"  polling=true;pollAt=Date.now();\n"
"  var rel=function(){polling=false};\n"
"  call('/api/status').then(rel,function(e){rel();offline(e)});\n"
"}\n"
/* Every 2 s, not every 1 s.

   Each poll is a whole TCP connection - the server answers Connection: close -
   and on lwIP a finished connection lingers in a pool of five
   (MEMP_NUM_TCP_PCB, the stock default, which this port does not raise). One a
   second keeps that pool at its edge, and a connection that cannot get a pcb is
   never accepted at all: the browser reports ERR_CONNECTION_TIMED_OUT while the
   serial log shows nothing, because nothing arrived.

   Halving the rate halves the churn. The charts hold 60 samples either way, so
   the history window becomes two minutes instead of one - not a loss for
   something read from a few metres away. */
"authRefresh();\n"
"setInterval(poll,2000);poll();\n"
"</script>\n"
"</body>\n"
"</html>\n";

#endif /* WEB_PAGE_H */
