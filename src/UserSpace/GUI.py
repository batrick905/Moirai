import tkinter as tk

class App(tk.Tk):
	def __init__(self):
		super().__init__()

		self.title("OS Project NFC Scan")
		self.geometry("800x480")

		self.configure(bg="#ffffff")

	def build_ui(self):
		pass

if __name__ == "__main__":
	app = App()
	app.build_ui()
	app.mainloop()
