import socket, threading, queue, time, tkinter as tk
from tkinter import ttk, scrolledtext
from PIL import Image, ImageTk
import cv2, numpy as np
import os, io
from datetime import datetime

# ======= 유틸 =======
def load_logo(path, max_size=(160, 60)):
    try:
        img = Image.open(path)
        img.thumbnail(max_size, Image.LANCZOS)
        return ImageTk.PhotoImage(img)
    except Exception:
        return None


def create_rounded_rectangle(canvas, x1, y1, x2, y2, radius=20, **kwargs):
    pts = []
    for x, y in [
        (x1, y1 + radius), (x1, y1), (x1 + radius, y1),
        (x2 - radius, y1), (x2, y1), (x2, y1 + radius),
        (x2, y2 - radius), (x2, y2), (x2 - radius, y2),
        (x1 + radius, y2), (x1, y2), (x1, y2 - radius)
    ]:
        pts.extend([x, y])
    return canvas.create_polygon(pts, smooth=True, **kwargs)


# ======= 상수 =======
REMOTE_DEF = "192.168.11.3"  # ArduCAM UDP streaming server IP
HDR = 4
WIN = "JPEG Stream"

# JPEG frame can be variable size
MAX_JPEG_SIZE = 120 * 1024  # 120KB


# ======= 패킷 재조립 =======
class Assembler:
    def __init__(self):
        self.buf, self.need = {}, {}

    def add(self, pkt: bytes):
        if len(pkt) <= HDR:
            return None
        fid, pid, tot = pkt[0], pkt[1], pkt[2]
        self.buf.setdefault(fid, {})[pid] = pkt[HDR:]
        self.need[fid] = tot
        if len(self.buf[fid]) == tot:
            data = b"".join(self.buf[fid].get(i, b"") for i in range(tot))
            self.buf.pop(fid, None)
            self.need.pop(fid, None)
            return data
        return None


# ======= 커스텀 버튼 =======
class AppleButton(tk.Canvas):
    def __init__(self, parent, text, command=None,
                 style="primary", state="normal", **kwargs):
        super().__init__(parent, highlightthickness=0, **kwargs)
        self.text, self.command = text, command
        self.style, self.state, self.is_hovered = style, state, False
        self.colors = {
            "primary":  {"bg": "#007AFF", "fg": "white", "hover": "#0056CC"},
            "secondary":{"bg": "#F2F2F7", "fg": "#1D1D1F", "hover": "#E5E5EA"},
            "danger":   {"bg": "#FF3B30", "fg": "white", "hover": "#D70015"},
            "success":  {"bg": "#34C759", "fg": "white", "hover": "#248A3D"},
        }
        self.bind("<Button-1>", self._on_click)
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)
        self.draw()

    def draw(self):
        self.delete("all")
        if self.state == "disabled":
            bg, fg = "#F2F2F7", "#8E8E93"
        else:
            cset = self.colors[self.style]
            bg = cset["hover"] if self.is_hovered else cset["bg"]
            fg = cset["fg"]
        create_rounded_rectangle(self, 2, 2,
                                 self.winfo_reqwidth()-2,
                                 self.winfo_reqheight()-2,
                                 radius=8, fill=bg, outline="")
        self.create_text(self.winfo_reqwidth()//2,
                         self.winfo_reqheight()//2,
                         text=self.text, fill=fg,
                         font=("Calibri", 11))

    def _on_click(self, _):
        if self.command and self.state == "normal":
            self.command()

    def _on_enter(self, _):
        if self.state == "normal":
            self.is_hovered = True
            self.after_idle(self.draw)

    def _on_leave(self, _):
        self.is_hovered = False
        self.after_idle(self.draw)

    def config_state(self, state):
        self.state = state
        self.draw()


# ======= 메인 앱 =======
class JPEGStreamApp:
    def __init__(self, root: tk.Tk):
        self.root, self.q = root, queue.Queue()
        self.sock, self.remote = None, None
        self.recv_th, self.scale = None, 4
        self.stop_all, self.stop_req = threading.Event(), threading.Event()
        self.streaming, self.window_up = False, False

        # frame/recording
        self.current_frame, self.frame_cnt = None, 0
        self.recording = False
        self.rec_writer, self.rec_start_time = None, None
        self.last_frame_time, self.fps_buffer, self.fps_est = None, [], 25.0

        self._setup_apple_style()
        self._build_ui()
        self._poll_log()
        self._start_fps_display_update()

    # ---------- 스타일 ----------
    def _setup_apple_style(self):
        st = ttk.Style()
        st.configure("Apple.TEntry", fieldbackground="#FFFFFF",
                     foreground="#1D1D1F", borderwidth=1, relief="solid",
                     bordercolor="#D1D1D6", insertcolor="#007AFF",
                     font=("Calibri", 11), padding=6)
        st.map("Apple.TEntry", bordercolor=[("focus", "#007AFF")])

    # ---------- UI ----------
    def _build_ui(self):
        self.root.title("ArduCAM JPEG Stream")
        self.root.geometry("600x680")
        self.root.configure(bg="#F2F2F7")

        main = tk.Frame(self.root, bg="#F2F2F7")
        main.pack(fill="both", expand=True, padx=10, pady=20)

        if (logo := load_logo("wiznet_logo.png")):
            tk.Label(main, image=logo, bg="#F2F2F7").pack()
            self.logo_img = logo

        tk.Label(main, text="ArduCAM JPEG Stream",
                 font=("SF Pro Display", 22, "bold"),
                 bg="#F2F2F7").pack()
        tk.Label(main, text="High-Speed JPEG Streaming over Ethernet",
                 font=("Calibri", 11), bg="#F2F2F7",
                 fg="#8E8E93").pack(pady=(0, 10))

        # ---- 입력·우측 패널 ----
        panel = tk.Frame(main, bg="#F2F2F7")
        panel.pack(fill="x")

        left = tk.Frame(panel, bg="#F2F2F7")
        right = tk.Frame(panel, bg="#F2F2F7")
        left.pack(side="left", fill="both", expand=True, padx=(0, 10))
        right.pack(side="left", fill="y")

        # 입력 필드
        self.v_rip, self.v_rpt = tk.StringVar(value=REMOTE_DEF), tk.StringVar(value="5000")
        self.v_lpt, self.v_scl = tk.StringVar(value="5000"), tk.StringVar(value="100")
        self.ents = []

        def add_field(parent, label, var):
            f = tk.Frame(parent, bg="#F2F2F7"); f.pack(fill="x", pady=3)
            tk.Label(f, text=label, font=("Calibri", 11),
                     bg="#F2F2F7").pack(anchor="w")
            e = ttk.Entry(f, textvariable=var, style="Apple.TEntry"); e.pack(fill="x")
            self.ents.append(e)

        for lbl, v in [
            ("ArduCAM IP Address", self.v_rip),
            ("ArduCAM Port", self.v_rpt),
            ("Local Port", self.v_lpt),
            ("Scale Percentage(25~200%)", self.v_scl),
        ]: add_field(left, lbl, v)

        # 해상도 선택 콤보
        tk.Label(right, text="Resolution", font=("Calibri", 11),
                 bg="#F2F2F7").pack(anchor="w")
        self.res_combo = ttk.Combobox(right, state="readonly",
                                      values=[
                                          "320x240", "640x480", "1280x720", 
                                          "1600x1200", "1920x1080"
                                      ],
                                      width=12)
        self.res_combo.current(0)  # Default to 320x240
        self.res_combo.pack(pady=(0, 8))
        self.res_combo.bind("<<ComboboxSelected>>", self._change_resolution)

        # 현재 프레임 표시
        tk.Label(right, text="Current Frame", font=("Calibri", 11),
                 bg="#F2F2F7").pack(anchor="w")
        self.frame_lab = tk.Label(right, text="0", font=("Calibri", 12, "bold"),
                                  bg="#F2F2F7")
        self.frame_lab.pack()

        # ---- 연결 버튼 ----
        btns = tk.Frame(main, bg="#F2F2F7")
        btns.pack(pady=8)

        self.b_conn = AppleButton(btns, "Connect", self._connect,
                                width=100, height=36)
        self.b_conn.pack(side="left", padx=5)

        self.b_clos = AppleButton(btns, "Disconnect", self._close_conn,
                                style="secondary", state="disabled",
                                width=100, height=36)
        self.b_clos.pack(side="left", padx=5)

        # ---- 스트리밍 제어 버튼 ----
        sbtns = tk.Frame(main, bg="#F2F2F7")
        sbtns.pack(pady=5)

        self.b_start = AppleButton(sbtns, "Start Stream", self._start,
                                style="success", state="disabled",
                                width=120, height=36)
        self.b_start.pack(side="left", padx=5)

        self.b_stop = AppleButton(sbtns, "Stop Stream", self._stop,
                                style="danger", state="disabled",
                                width=120, height=36)
        self.b_stop.pack(side="left", padx=5)

        # ---- 캡쳐/녹화 버튼 ----
        crbtns = tk.Frame(main, bg="#F2F2F7")
        crbtns.pack(pady=5)

        self.b_cap = AppleButton(crbtns, "Capture", self._capture,
                                style="secondary", state="disabled",
                                width=120, height=36)
        self.b_cap.pack(side="left", padx=5)

        self.b_rec = AppleButton(crbtns, "REC", self._toggle_record,
                                style="danger", state="disabled",
                                width=120, height=36)
        self.b_rec.pack(side="left", padx=5)

        self.rec_status = tk.Label(crbtns, text="", font=("Calibri", 11),
                                bg="#F2F2F7", fg="#FF3B30")
        self.rec_status.pack(side="left", padx=5)

        # 상태 표시
        self.status_dot = tk.Canvas(main, width=12, height=12,
                                    bg="#F2F2F7", highlightthickness=0)
        self.status_dot.create_oval(2, 2, 10, 10, fill="#8E8E93", outline="")
        self.status_dot.pack()
        self.status_txt = tk.Label(main, text="Disconnected",
                                   font=("Calibri", 13), bg="#F2F2F7")
        self.status_txt.pack()

        # 로그
        self.log = scrolledtext.ScrolledText(main, height=6,
                                             font=("SF Mono", 10),
                                             bg="#FFFFFF")
        self.log.pack(fill="both", expand=True, pady=(5, 0))
        self.log.insert(tk.END, "Welcome to ArduCAM JPEG Stream\nReady to connect.\n\n")

    # ---------- 로그 ----------
    def _set_state(self, txt, color):
        self.status_txt.config(text=txt)
        self.status_dot.delete("all")
        self.status_dot.create_oval(2, 2, 10, 10, fill=color, outline="")
        
    def _start_fps_display_update(self):
        def update():
            fps = self.frame_cnt
            self.frame_cnt = 0  # 초기화
            self.frame_lab.config(text=f"{fps} fps")
            self.root.after(1000, update)

        self.root.after(1000, update)

    def _log(self, msg):
        self.q.put(f"[{time.strftime('%H:%M:%S')}] {msg}")

    def _poll_log(self):
        while not self.q.empty():
            self.log.insert(tk.END, self.q.get() + "\n")
            self.log.see(tk.END)
        self.root.after(80, self._poll_log)

    # ---------- 해상도 변경 ----------
    def _change_resolution(self, _):
        sel = self.res_combo.get()
        
        if not self.sock:
            self._log(f"Not connected - Resolution will be set to {sel} on connection")
            return
            
        # Convert GUI format to command format
        res_map = {
            "320x240": "RES_320X240",
            "640x480": "RES_640X480", 
            "1280x720": "RES_1280X720",
            "1600x1200": "RES_1600X1200",
            "1920x1080": "RES_1920X1080"
        }
        
        if sel in res_map:
            cmd = res_map[sel]
            try:
                # 스트리밍 중이면 잠시 멈추고 해상도 변경
                was_streaming = self.streaming
                if was_streaming:
                    self._log(f"Stopping stream for resolution change to {sel}")
                    self.sock.sendto(b"STOP", self.remote)
                    time.sleep(0.5)  # 서버가 STOP을 처리할 시간
                
                # 해상도 변경 명령 전송
                self._log(f"Sending resolution command: {cmd}")
                self.sock.sendto(cmd.encode(), self.remote)
                time.sleep(0.2)  # 해상도 변경 처리 시간
                
                # 스트리밍이 활성화되어 있었다면 다시 시작
                if was_streaming:
                    self._log(f"Restarting stream with new resolution: {sel}")
                    self.sock.sendto(b"START", self.remote)
                    
                self._log(f"Resolution changed to: {sel}")
                
            except Exception as e:
                self._log(f"Failed to change resolution: {e}")
        else:
            self._log(f"Invalid resolution: {sel}")

    # ---------- 연결 ----------
    def _connect(self):
        try:
            rip = self.v_rip.get().strip(); socket.inet_aton(rip)
            rpt, lpt = int(self.v_rpt.get()), int(self.v_lpt.get())
            if not (1 <= rpt <= 65535 and 1 <= lpt <= 65535):
                raise ValueError("port")
        except Exception:
            self._set_state("Invalid Input", "#FF3B30"); return

        try:
            pct = max(25, min(200, int(self.v_scl.get())))
        except ValueError:
            pct = 100
        self.scale, self.v_scl.set(str(pct))
        self.scale = max(1, min(8, round(pct/25)))

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", lpt)); self.sock.settimeout(0.1)
        self.remote = (rip, rpt)

        if not self.recv_th or not self.recv_th.is_alive():
            self.stop_all.clear()
            self.recv_th = threading.Thread(target=self._recv_loop, daemon=True)
            self.recv_th.start()

        for e in self.ents: e.config(state="disabled")
        self.b_conn.config_state("disabled")
        self.b_clos.config_state("normal")
        self.b_start.config_state("normal")
        self.res_combo.config(state="readonly")  # Keep readonly but allow changes when connected
        self._set_state("Connected", "#34C759")
        self._log(f"Connected to {rip}:{rpt}")
        
        # Send initial resolution setting - 연결 후 바로 설정
        sel = self.res_combo.get()
        res_map = {
            "320x240": "RES_320X240", 
            "640x480": "RES_640X480", 
            "1280x720": "RES_1280X720",
            "1600x1200": "RES_1600X1200", 
            "1920x1080": "RES_1920X1080"
        }
        if sel in res_map:
            cmd = res_map[sel]
            try:
                time.sleep(0.2)  # 연결 안정화 대기
                self._log(f"Setting initial resolution: {cmd}")
                self.sock.sendto(cmd.encode(), self.remote)
                self._log(f"Initial resolution command sent: {sel}")
            except Exception as e:
                self._log(f"Failed to set initial resolution: {e}")

    def _close_conn(self):
        self._stop()
        if self.sock: self.sock.close(); self.sock = None
        for e in self.ents: e.config(state="normal")
        self.b_conn.config_state("normal"); self.b_clos.config_state("disabled")
        self.b_start.config_state("disabled"); self.b_cap.config_state("disabled")
        self.b_rec.config_state("disabled"); self.rec_status.config(text="")
        self.res_combo.config(state="readonly")  # Re-enable resolution selection
        self._set_state("Disconnected", "#8E8E93"); self._log("Disconnected")
        self.stop_req = threading.Event()  # 스트리밍 재시작 대비

    # ---------- 스트리밍 ----------
    def _start(self):
        if not self.sock or self.streaming: return
        self.sock.sendto(b"START", self.remote)
        self.streaming, self.stop_req = True, threading.Event()
        self.b_start.config_state("disabled"); self.b_stop.config_state("normal")
        self.b_cap.config_state("normal"); self.b_rec.config_state("normal")
        self._set_state("Streaming", "#007AFF"); self._log("Stream started")

    def _stop(self):
        if not self.streaming: return
        try: self.sock.sendto(b"STOP", self.remote)
        except OSError: pass
        self.streaming = False
        self.stop_req.set()
        self.b_start.config_state("normal"); self.b_stop.config_state("disabled")
        if self.recording: self._toggle_record()
        self.b_cap.config_state("disabled"); self.b_rec.config_state("disabled")
        self._set_state("Connected", "#34C759"); self._log("Stream stopped")

    # ---------- 캡쳐/녹화 ----------
    def _capture(self):
        if self.current_frame is None or not self.streaming or self.recording: return
        fname = f"jpeg_capture_{datetime.now():%Y%m%d_%H%M%S}.jpg"
        cv2.imwrite(os.path.join(os.getcwd(), fname), self.current_frame)
        self._log(f"Captured to {fname}")

    def _toggle_record(self):
        if not self.streaming: return
        if not self.recording:
            fname = f"jpeg_record_{datetime.now():%Y%m%d_%H%M%S}.mp4"
            fourcc = cv2.VideoWriter_fourcc(*"mp4v")
            fps = max(1.0, min(60.0, self.fps_est))
            h, w = self.current_frame.shape[:2] if self.current_frame is not None else (240, 320)
            self.rec_writer = cv2.VideoWriter(fname, fourcc, fps, (w, h))
            if not self.rec_writer.isOpened(): self._log("VideoWriter fail"); return
            self.recording, self.rec_start_time = True, time.time()
            self.b_cap.config_state("disabled")
            self.rec_status.config(text="● 00:00")
            self._update_rec_time(); self._log(f"REC start {fps:.1f}fps")
        else:
            self.recording = False
            if self.rec_writer: self.rec_writer.release(); self.rec_writer=None
            self.b_cap.config_state("normal")
            self.rec_status.config(text=""); self._log("REC saved")

    def _update_rec_time(self):
        if not self.recording: return
        mm, ss = divmod(int(time.time()-self.rec_start_time), 60)
        self.rec_status.config(text=f"● {mm:02d}:{ss:02d}")
        self.root.after(500, self._update_rec_time)

    # ---------- 수신 ----------
    def _recv_loop(self):
        asm = Assembler()
        while not self.stop_all.is_set():
            if self.stop_req.is_set(): self._safe_destroy(); time.sleep(0.05); continue
            if not (self.sock and self.streaming): time.sleep(0.05); continue
            try:
                data, _ = self.sock.recvfrom(4096)
                
                # Check for resolution acknowledgment
                if data == b"RES_OK":
                    current_res = self.res_combo.get()
                    self._log(f"✓ Resolution change confirmed: {current_res}")
                    continue
                    
                frame = asm.add(data)
                if frame and self.streaming: 
                    # 프레임 크기 디버깅
                    if len(frame) > 10:
                        self._log(f"Frame received: {len(frame)} bytes, resolution: {self.res_combo.get()}")
                    self._show_frame(frame)
                    
            except socket.timeout: continue
            except Exception as e:
                self._log(f"Recv err: {e}"); self.root.after(0, self._stop)
        self._safe_destroy(); self._log("Receiver end")

    # ---------- 프레임 표시 ----------
    def _show_frame(self, jpeg_data: bytes):
        if self.stop_req.is_set(): return
        now = time.time()
        if self.last_frame_time:
            self.fps_buffer.append(1.0/max(1e-6, now-self.last_frame_time))
            if len(self.fps_buffer) > 30: self.fps_buffer.pop(0)
            self.fps_est = sum(self.fps_buffer)/len(self.fps_buffer)
        self.last_frame_time = now

        try:
            # Decode JPEG data
            nparr = np.frombuffer(jpeg_data, np.uint8)
            bgr = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            
            if bgr is not None:
                h, w = bgr.shape[:2]
                # 실제 이미지 크기 로깅 (첫 몇 프레임만)
                if self.frame_cnt < 3:
                    self._log(f"Decoded image size: {w}x{h}, expected: {self.res_combo.get()}")
                
                if not self.window_up:
                    cv2.namedWindow(WIN, cv2.WINDOW_NORMAL); self.window_up = True
                
                # Scale image if needed
                if self.scale != 4:  # Default scale factor from original stream_viewer
                    new_w, new_h = int(w * self.scale / 4), int(h * self.scale / 4)
                    bgr = cv2.resize(bgr, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
                
                self.current_frame = bgr
                if self.recording and self.rec_writer: self.rec_writer.write(bgr)
                cv2.imshow(WIN, bgr); cv2.waitKey(1)

                # 프레임 카운트 UI
                self.frame_cnt += 1
        except Exception as e:
            self._log(f"JPEG decode error: {e}")

    # ---------- 종료 ----------
    def _safe_destroy(self):
        if self.window_up:
            try: cv2.destroyWindow(WIN)
            except cv2.error: pass
            self.window_up = False

    def on_quit(self):
        self.stop_all.set(); self._close_conn(); self.root.destroy()


# ======= 실행 =======
if __name__ == "__main__":
    root = tk.Tk()
    app = JPEGStreamApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_quit)
    root.mainloop()
