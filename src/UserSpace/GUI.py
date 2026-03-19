mport tkinter as tk
from tkinter.scrolledtext import ScrolledText
import threading
import paramiko
import time

PI_HOST     = "10.121.63.187"
PI_USER     = "admin"
PI_PASSWORD = "1234"
PI_KEY      = None
PI_LOG_PATH = "/home/admin/Documents/Moirai/src/KernelSpace/taps.log"
POLL_SEC    = 2

def fetch_log():
    try:
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        if PI_KEY:
            client.connect(PI_HOST, username=PI_USER, key_filename=PI_KEY)
        else:
            client.connect(PI_HOST, username=PI_USER, password=PI_PASSWORD)
        sftp = client.open_sftp()
        with sftp.open(PI_LOG_PATH, "r") as f:
            content = f.read().decode("utf-8")
        sftp.close()
        client.close()
        return content
    except Exception as e:
        return f"[connection error: {e}]"

class App(tk.Tk):
    def _init_(self):
        super()._init_()
        self.title("Moirai — NFC Access Log")
        self.geometry("800x480")
        self.configure(bg="#ffffff")
        self.last_content = ""

    def build_ui(self):
        tk.Label(
            self, text="NFC ACCESS LOG",
            bg="#ffffff", fg="#000000",
            font=("Monospace", 24, "bold")
        ).place(x=210, y=30)

        tk.Label(
            self, text=f"Pi: {PI_HOST}  •  {PI_LOG_PATH}",
            bg="#ffffff", fg="#888888",
            font=("Monospace", 9)
        ).place(x=150, y=75)

        self.log_box = ScrolledText(
            self, state="disabled",
            bg="#111111", fg="#00ff00",
            font=("Monospace", 10),
            relief="flat"
        )
        self.log_box.place(x=50, y=100, width=700, height=300)

        self.status = tk.Label(
            self, text="Connecting...",
            bg="#ffffff", fg="#888888",
            font=("Monospace", 9)
        )
        self.status.place(x=50, y=415)

        self.poll()

    def poll(self):
        threading.Thread(target=self._fetch_and_update, daemon=True).start()

    def _fetch_and_update(self):
        content = fetch_log()
        self.after(0, self._update_ui, content)

    def _update_ui(self, content):
        if content != self.last_content:
            self.last_content = content
            self.log_box.configure(state="normal")
            self.log_box.delete("1.0", "end")
            self.log_box.insert("end", content)
            self.log_box.configure(state="disabled")
            self.log_box.see("end")

        if "[connection error" in content:
            self.status.configure(fg="red", text=f"Error — retrying in {POLL_SEC}s")
        else:
            self.status.configure(fg="#00aa00", text=f"Connected  •  last updated {time.strftime('%H:%M:%S')}")

        self.after(POLL_SEC * 1000, self.poll)

if _name_ == "_main_":
    app = App()
    app.build_ui()
    app.mainloop()
