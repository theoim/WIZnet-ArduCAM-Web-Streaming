/**
 * @file    web_page.h
 * @brief   The panel's markup, styles and script, as two string constants.
 *
 * Lifted out of webserver.c, which was fifty kilobytes with three hundred lines
 * of HTML in the middle of its socket handling. Two different kinds of work in
 * one file is survivable; what is not is that every CSS change meant counting
 * escaped quotes inside C string literals, and a rule that landed in the wrong
 * place was invisible until the page rendered.
 *
 * The split is by what the text is, not by what it does:
 *
 *   PAGE_HEAD     headers, <style> and the whole body. No C needed.
 *   PAGE_SCRIPT   everything after the few runtime values the page cannot
 *                 work out for itself.
 *
 * Between the two, webserver.c still writes the token and the stand-down time
 * with snprintf - they come from config.h and from the detector, so they are
 * the one part that cannot be a constant.
 *
 * Included once, by webserver.c. The arrays are static on purpose: a second
 * translation unit including this would get its own copy, and the point is that
 * there is only one page.
 */

#ifndef __WEB_PAGE_H__
#define __WEB_PAGE_H__

/* ---------------------------------------------------------------- markup */
static const char PAGE_HEAD[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Tank</title><style>"
        "body{margin:0;background:#111;color:#ddd;"
        "font:15px -apple-system,system-ui,sans-serif}"
        "header{padding:12px 14px;background:#1b1b1b;font-weight:600}"
        "nav{display:flex;background:#1b1b1b;border-bottom:1px solid #333}"
        "nav button{flex:1;padding:12px;background:none;border:none;color:#888;"
        "font:inherit;border-bottom:2px solid transparent}"
        "nav button.a{color:#fff;border-bottom-color:#2ecc71}"
        ".s{display:flex;align-items:center;gap:10px;padding:10px 14px;"
        "border-bottom:1px solid #222}"
        ".s span{flex:0 0 92px;color:#999;font-size:13px}"
        ".bar{flex:1;height:14px;border-radius:7px;background:#2a2a2a;"
        "transition:background .25s}"
        ".on-run{background:#2ecc71}"
        ".on-buz{background:#ff3b30;animation:p 1s infinite}"
        "@keyframes p{50%{opacity:.45}}"
        ".v{color:#777;font-size:12px;flex:0 0 auto;"
        "font-variant-numeric:tabular-nums}"
        "#box{background:#000;min-height:180px;display:flex;"
        "align-items:center;justify-content:center;text-align:center;"
        "color:#666;padding:28px 18px;font-size:14px;line-height:1.6}"
        "#box img{width:100%;height:auto;display:block}"
        "p.n{padding:10px 14px;color:#777;font-size:12px;margin:0}"
        ".f{padding:10px 14px;border-bottom:1px solid #222}"
        ".f label{display:block;color:#999;font-size:12px;margin-bottom:5px}"
        ".f input{width:100%;box-sizing:border-box;padding:9px 10px;"
        "background:#1d1d1d;border:1px solid #333;border-radius:6px;"
        "color:#eee;font:14px ui-monospace,monospace}"
        ".row{display:flex;gap:10px}.row>div{flex:1}"
        ".rs{display:flex;gap:8px}"
        ".rs button{flex:1;padding:11px;background:#1d1d1d;"
        "border:1px solid #333;border-radius:6px;color:#aaa;font:inherit}"
        ".rs button.a{background:#16281c;border-color:#2ecc71;color:#fff}"
        ".rs small{color:#777;font-size:11px}"
        ".cur{margin-top:8px;padding:8px 10px;background:#15201a;"
        "border-left:2px solid #2ecc71;border-radius:4px;color:#9bbfa8;"
        "font:12px ui-monospace,monospace;line-height:1.7}"
        ".act{display:flex;gap:10px;padding:14px}"
        ".act button{flex:1;padding:13px;border:none;border-radius:8px;"
        "font:600 15px inherit}"
        "#save{background:#2ecc71;color:#06220f}"
        "#ref{background:#2a2a2a;color:#ccc}"
        "#msg{padding:0 14px 16px;font-size:13px;color:#888;line-height:1.5}"
        ".h{color:#666;font-size:11px;margin-top:5px;line-height:1.5}"
        ".hide{display:none}"
        /* The meter is a filled bar with a tick where the threshold sits, so
           "is this sound close to firing" is answered by looking rather than by
           comparing two numbers in different units. */
        ".mt{position:relative;height:20px;border-radius:4px;background:#1d1d1d;"
        "border:1px solid #333;overflow:hidden;margin-top:6px}"
        ".mt i{position:absolute;left:0;top:0;bottom:0;background:#2ecc71;"
        "transition:width .25s}"
        ".mt u{position:absolute;top:-2px;bottom:-2px;width:2px;"
        "background:#ff3b30;text-decoration:none}"
        ".mt.over i{background:#ff3b30}"
        ".rd{display:flex;justify-content:space-between;align-items:baseline;"
        "font:12px ui-monospace,monospace;color:#888;margin-top:5px}"
        ".rd b{color:#eee;font-size:16px;font-weight:600}"
        "input[type=range]{width:100%;margin:10px 0 0;accent-color:#2ecc71}"
        ".sv{color:#2ecc71;font:600 13px ui-monospace,monospace}"
        /* Every property a nav button can inherit is named, because styling by
           subtraction leaves whatever was not mentioned - which rendered the
           third tab with a box around it and its label cut in half. */
        "nav button{appearance:none;-webkit-appearance:none;"
        "flex:1 1 0;min-width:0;box-sizing:border-box;"
        "margin:0;padding:0 8px;height:46px;line-height:44px;"
        "background:transparent;border:0;border-radius:0;"
        "border-bottom:2px solid transparent;"
        "color:#888;font:inherit;white-space:nowrap;overflow:hidden;"
        "text-overflow:ellipsis;cursor:pointer}"
        "nav button.a{color:#fff;border-bottom-color:#2ecc71}"
        "nav button:focus{outline:none}"
        "nav button:focus-visible{outline:2px solid #2ecc71;outline-offset:-4px}"
        "</style></head><body>"
        "<header>저수조 모니터</header>"
        "<nav><button id=\"tl\" class=\"a\">LIVE</button>"
        "<button id=\"tn\">NETWORK</button>"
        "<button id=\"tm\">MIC / CAM</button></nav>"

        "<div id=\"live\">"
        "<div class=\"s\"><span>RUN</span><div id=\"r\" class=\"bar\"></div>"
        "<div class=\"v\" id=\"rv\">-</div></div>"
        "<div class=\"s\"><span>BUZZER</span><div id=\"b\" class=\"bar\"></div>"
        "<div class=\"v\" id=\"bv\">-</div></div>"
        "<div id=\"box\"></div>"
        "<p class=\"n\" id=\"note\"></p>"
        "</div>"

        "<div id=\"net\" class=\"hide\">"
        "<div class=\"f\"><label>주소 방식</label>"
        "<div class=\"rs\">"
        "<button id=\"m1\">DHCP <small>자동</small></button>"
        "<button id=\"m0\">STATIC <small>고정</small></button></div>"
        "<div class=\"h\">아래 네 칸은 지금 동작 중인 값입니다. "
        "DHCP는 공유기에서 주소를 받고, STATIC은 아래 값을 그대로 씁니다. "
        "바꾸면 SAVE 후 재시작해야 적용됩니다.</div></div>"
        "<div class=\"f\"><label>내부 IP</label><input id=\"ip\"></div>"
        "<div class=\"f row\">"
        "<div><label>서브넷</label><input id=\"sn\"></div>"
        "<div><label>게이트웨이</label><input id=\"gw\"></div></div>"
        "<div class=\"f row\">"
        "<div><label>DNS</label><input id=\"dns\"></div>"
        "<div><label>내부 포트</label><input id=\"port\"></div></div>"
        "<div class=\"f row\">"
        "<div><label>외부 IP</label><input id=\"ph\"></div>"
        "<div><label>외부 포트</label><input id=\"pp\"></div></div>"
        "<div class=\"f\"><label>Discord Webhook URL</label>"
        "<input id=\"wh\" autocomplete=\"off\">"
        "<div class=\"h\" id=\"whs\"></div></div>"
        "<div class=\"act\"><button id=\"ref\">REFRESH</button>"
        "<button id=\"save\">SAVE</button></div>"
        "<div id=\"msg\"></div>"
        "</div>"

        /* ------------------------------------------------- mic / camera ---
           A meter above a slider, updating several times a second whether or
           not anything is being adjusted.

           The right threshold is not a number from a manual. It is the gap
           between what this room sounds like and what its buzzer sounds like,
           and nobody knows that gap until they stand in the room and look at
           it. So: show the live level, mark where the threshold sits, and let
           the slider move the mark. */
        "<div id=\"mic\" class=\"hide\">"

        "<div class=\"f\"><label>경보 기준 소리 (dB)</label>"
        "<div class=\"mt\" id=\"dm\"><i id=\"df\"></i><u id=\"dt\"></u></div>"
        "<div class=\"rd\"><span>지금 <b id=\"dv\">-</b> dB</span>"
        "<span id=\"dbase\">-</span></div>"
        "<input type=\"range\" id=\"ds\" min=\"-70\" max=\"-10\" step=\"1\">"
        "<div class=\"h\">경보 기준 <span class=\"sv\" id=\"dsv\">-</span> dB "
        "&nbsp;·&nbsp; 빨간 선이 기준입니다. <b>부저를 울렸을 때 막대가 선을 "
        "넘고, 조용할 때 넘지 않는 자리</b>로 맞추세요. "
        "이보다 작은 소리는 아무리 오래 가도 경보가 되지 않습니다.</div></div>"

        "<p class=\"n\">마이크 입력 <b id=\"blk\">-</b> "
        "<small>(정상이면 62 blk/s 근처입니다. 0이면 마이크를 못 읽는 "
        "중입니다.)</small><br>주 주파수 <b id=\"fhz\">-</b></p>"

        "<p class=\"n\">슬라이더는 놓는 즉시 반영됩니다. 재시작 후에도 "
        "유지하려면 아래 SAVE를 누르세요.</p>"

        "<div class=\"f\"><label>카메라 해상도</label>"
        "<div class=\"rs\">"
        "<button id=\"r1\">QVGA <small>320&times;240</small></button>"
        "<button id=\"r2\">VGA <small>640&times;480</small></button>"
        "<button id=\"r3\">HD <small>1280&times;720</small></button></div>"
        "<div class=\"h\">누르면 바로 적용됩니다. 재시작 후에도 그 해상도로 "
        "올라오게 하려면 SAVE 하세요. 해상도가 높을수록 한 장을 찍어 보내는 "
        "시간이 길어져 영상이 느려집니다.</div></div>"

        "<div class=\"act\"><button id=\"ref2\">REFRESH</button>"
        "<button id=\"save2\">SAVE</button></div>"
        "<div id=\"msg2\"></div>"
        "</div>"

        "<script>";

/* ---------------------------------------------------------------- script */
static const char PAGE_SCRIPT[] =
        "var live=1,tabn=0,RES=1,armed=0;"
        "function q(i){return document.getElementById(i)}"

        /*
         * The video starts with the page and is never taken down.
         *
         * It used to be swapped in and out by the alarm, which meant the one
         * moment someone most wanted to look - walking up to the box to find out
         * why it had been quiet - was a moment with nothing to see. A control
         * panel that only works during an emergency cannot be checked.
         *
         * Reloading the <img> is also how the stream recovers: if the connection
         * dies the browser fires onerror, and a fresh src with a new cache-buster
         * asks for another one.
         */
        "function startStream(){q('box').innerHTML="
        "'<img id=\"v\" src=\"/stream?t='+T+'&c='+Date.now()+'\">';"
        "q('v').onerror=function(){setTimeout(startStream,2000)};}"

        /* The lit button is the stored value - there is no hidden field and no
         * third state. Whatever the board reported is what shows, so the current
         * mode can be read off the panel without pressing anything. */
        /*
         * Unlike the resolution buttons, these do not act at once. The address
         * is what every listening socket was opened on and what the phone is
         * connected through, so changing it means a restart - and a restart that
         * happens the moment somebody taps a button, before they have been told
         * what the new address will be, is a box that disappears mid-sentence.
         * SAVE is where that happens, with a message first.
         */
        "var MODE=0;"
        "function setMode(v){MODE=v;"
        "q('m1').className=(v?'a':'');q('m0').className=(v?'':'a');}"
        "q('m1').onclick=function(){setMode(1)};"
        "q('m0').onclick=function(){setMode(0)};"

        "function setRes(v){RES=v;"
        "q('r1').className=(v==1?'a':'');q('r2').className=(v==2?'a':'');"
        "q('r3').className=(v==3?'a':'');}"

        /* Pressing a button changes the sensor there and then, and the video is
           reloaded because the connection it was on was dropped to let the
           sensor be reconfigured. SAVE is what makes the choice survive a power
           cut; it is not what makes it happen. */
        "function pickRes(v){setRes(v);q('msg').textContent='해상도 변경 중...';"
        "fetch('/api/res?v='+v+'&t='+T,{cache:'no-store'})"
        ".then(function(r){return r.text()}).then(function(t){"
        "q('msg').textContent=t+' 적용됨';startStream();})"
        ".catch(function(){q('msg').textContent='해상도 변경 실패';});}"
        "q('r1').onclick=function(){pickRes(1)};"
        "q('r2').onclick=function(){pickRes(2)};"
        "q('r3').onclick=function(){pickRes(3)};"
        /* `live` still means "the video is on screen", which decides whether
           the stream element exists. `tabn` is which panel is showing - the
           microphone tab keeps polling with the video hidden, because its
           meter is the whole point of it. */
        "function tab(n){tabn=n;live=(n==0);"
        "q('tl').className=n==0?'a':'';q('tn').className=n==1?'a':'';"
        "q('tm').className=n==2?'a':'';"
        "q('live').className=n==0?'':'hide';"
        "q('net').className=n==1?'':'hide';"
        "q('mic').className=n==2?'':'hide';"
        "if(n==1)loadCfg();}"
        "q('tl').onclick=function(){tab(0)};"
        "q('tn').onclick=function(){tab(1)};"
        "q('tm').onclick=function(){tab(2)};"

        /* Polling stops while the config tab is open. Nothing on that tab shows
         * live state, and a request every second would compete for the same
         * four sockets as the save. */
        /* Only the network tab stops polling: nothing on it moves, and a
           request a second would compete with the save for the same sockets. */
        "function poll(){if(tabn==1)return;"
        "fetch('/api/status?t='+T,{cache:'no-store'}).then(function(r){"
        "return r.json()}).then(function(s){"
        "q('r').className='bar on-run';q('rv').textContent=s.up+'s';"
        "q('b').className=s.buzzer?'bar on-buz':'bar';"
        "q('bv').textContent=s.buzzer?(s.hz+'Hz'):('tone '+s.tone);"
        "q('note').textContent=s.buzzer?NOTE:'';"
        "meter(s);"
        "}).catch(function(){q('r').className='bar';"
        "q('rv').textContent='응답 없음';});}"

        /* The bar and the tick share one scale, -70 dB to -10 dB, which is the
           slider's range: the mark can then never sit off the end where it
           could not be judged. */
        "function meter(s){"
        "if(!armed&&s.fl){q('ds').value=s.fl;armed=1;}"
        "var thr=parseFloat(q('ds').value);"
        "var lo=-70,hi=-10,sc=function(v){"
        "return Math.max(0,Math.min(100,(v-lo)/(hi-lo)*100))};"
        "q('df').style.width=sc(s.db)+'%';"
        "q('dt').style.left=sc(thr)+'%';"
        "q('dm').className=s.db>=thr?'mt over':'mt';"
        "q('dv').textContent=s.db.toFixed(0);"
        "q('dsv').textContent=thr.toFixed(0);"
        "q('dbase').textContent='평소 '+s.basedb.toFixed(0)+' dB';"
        "q('fhz').textContent=s.hz+' Hz';"
        "q('blk').textContent=s.blk+' blk/s';"
        "q('blk').style.color=s.blk>40?'#2ecc71':'#ff3b30';}"

        /* Sent on release, not on every pixel of the drag: each one is a
           request and the board has six sockets. */
        "function pushMic(){q('msg2').textContent='적용 중...';"
        "fetch('/api/mic?fl='+q('ds').value+'&t='+T)"
        ".then(function(r){return r.text()}).then(function(t){"
        "q('msg2').textContent=t+' — 재시작 후에도 쓰려면 SAVE 하세요.';})"
        ".catch(function(){q('msg2').textContent='적용 실패';});}"
        "q('ds').onchange=pushMic;"

        "function loadCfg(){"
        "fetch('/api/config?t='+T,{cache:'no-store'}).then(function(r){"
        "return r.json()}).then(function(c){"
        "q('ip').value=c.ip;q('sn').value=c.sn;q('gw').value=c.gw;"
        "q('dns').value=c.dns;q('port').value=c.port;"
        "q('ph').value=c.ph;q('pp').value=c.pp;setRes(c.res);"
        "setMode(c.dhcp);"
        "q('whs').textContent=c.whset?"
        "'현재 저장되어 있습니다. 비워두면 그대로 유지됩니다.':"
        "'설정되어 있지 않습니다. 알림을 받으려면 URL을 붙여넣으세요.';"
        "q('wh').value='';q('db').value=c.fl?c.fl:'';"
        "q('msg').textContent='';});}"

        "q('ref').onclick=function(){q('msg').textContent='불러오는 중...';"
        "loadCfg();};"

        /* Both tabs save the whole settings record, because that is what it is
           in flash - a partial write is not a thing. Which button was pressed
           only decides which line the answer appears on. */
        "function doSave(where){"
        "var p=['ip','sn','gw','dns','port','ph','pp','wh'].map(function(k){"
        "return k+'='+encodeURIComponent(q(k).value)}).join('&')"
        "+'&res='+RES+'&dhcp='+MODE+'&db='+q('ds').value;"
        "q(where).textContent='저장 중...';"
        "fetch('/api/save?t='+T,{method:'POST',body:p}).then(function(r){"
        "return r.text()}).then(function(t){q(where).textContent=t;"
        /* The board restarts after a save because the address it listens on is
         * one of the things being changed. Saying so beats a page that simply
         * stops answering. */
        "}).catch(function(){q(where).textContent="
        "'전송 실패 - 이미 재시작했을 수 있습니다.';});}"
        "q('save').onclick=function(){doSave('msg')};"
        "q('save2').onclick=function(){doSave('msg2')};"
        "q('ref2').onclick=function(){armed=0;q('msg2').textContent="
        "'보드에 저장된 값을 다시 읽었습니다.';};"

        /* Three times a second while the meter is on screen, once a second
           otherwise. Setting a threshold means making a noise and watching the
           bar answer, and a bar that answers a second later cannot be aimed. */
        "function beat(){poll();setTimeout(beat,tabn==2?300:1000);}"
        "startStream();loadCfg();beat();"
        "</script></body></html>";

#endif /* __WEB_PAGE_H__ */
