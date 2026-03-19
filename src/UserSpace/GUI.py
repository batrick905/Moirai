import tkinter as tk
from tkinter.scrolledtext import ScrolledText
import tkinter.simpledialog as simpledialog
import os

LOG_FILE = "a"

class App(tk.Tk):
	def __init__(self):
		super().__init__()

		self.title("OS Project NFC Scan")
		self.geometry("800x480")

		self.configure(bg="#ffffff")

	def register_uid(self, uid, db):
		#adding name popup
		name = simpledialog.askstring(
			"New Card Detected",
			f"UID: {uid}\n\nEnter a name for this card (or cancel to skip):"
		)
		if name and name.strip():
			db[uid] = name.strip()
			save_db(db)
		return db

	def build_ui(self):
		#text box
		tk.Label(self, text="NFC LOG", bg= "#ffffff", fg="#000000", font=("Monospace", 32)).place(x=275, y=50)

		#box with nfc names
		self.log_box = ScrolledText(self, state="disabled", bg="#111111", fg="#00ff00", font=("Monospace", 10))
		self.log_box.place(x=150, y=100, width=450, height=150)
		self.refresh_log()

	def refresh_log(self):
		if os.path.exists(LOG_FILE):
			with open(LOG_FILE, "r") as f:
				content = f.read()
			self.log_box.configure(state="normal")
			self.log_box.delete("1.0", "end")
			self.log_box.insert("end", content)
			self.log_box.configure(state = "disabled")
			self.log_box.see("end") #scrolls to last added name
		self.after(1000, self.refresh_log) # checks every second

if __name__ == "__main__":
	app = App()
	app.build_ui()
	app.mainloop()
