import tkinter as tk

class Kernel:
    def __init__(self, name):
        self.name = name

class Application(tk.Tk):
    def __init__(self, name):
        super().__init__()
        self.name = name
        self.kernel = None

    def set_kernel(self, kernel):
        self.kernel = kernel

    def run(self):
        if self.kernel:
            print(f"{self.name} is running with kernel {self.kernel.name}")
        else:
            print(f"{self.name} is running without a kernel")

app = Application("MyApp")
app.set_kernel(Kernel("DefaultKernel"))
app.run()
app.mainloop()