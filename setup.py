import platform
import subprocess

if __name__ == "__main__":
  if platform.system() == "Windows":
    subprocess.call("cmake -B build")
  elif platform.system() == "Darwin":
    subprocess.call("cmake -B build -G \"Xcode\"", shell=True)
  else:
    print("Error: Unsupported operating system!")
