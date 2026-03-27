# Desc: Entry point for RPCortex - Nebula OS
# File: /main.py
# Last Updated: 3/26/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta2
# Author: dash1101


# RPC β81

VERSION = "β81"
CODENAME = "Nebula"

_R = "\033[0m"

def _grad(text_lines):

    gradient = [
        "\033[96m",   # bright cyan
        "\033[36m",   # cyan
        "\033[36m",   # cyan
        "\033[94m",   # bright blue
        "\033[34m",   # blue
        "\033[95m",   # bright magenta
        "\033[35m",   # magenta
    ]
    for i, line in enumerate(text_lines):
        color = gradient[i % len(gradient)]
        print(color + line + _R)

def main():
    LOGO = [
        "      :::::::::  :::::::::   ::::::::::",
        "     :+:    :+: :+:    :+: :+:    :+: ",
        "    +:+    +:+ +:+    +:+ +:+         ",
        "   +#++:++#:  +#++:++#+  +#+          ",
        "  +#+    +#+ +#+        +#+           ",
        " #+#    #+# #+#        #+#    #+#     ",
        "###    ### ###         ########       ",
    ]

    print()
    _grad(LOGO)
    
    ver_str = "RPCortex {} - {}".format(VERSION, CODENAME)
    print("\033[90m" + ver_str.center(41) + _R)
    print("\033[90m" + ("─" * 41) + _R)
    print("Initializing OS...\n")

    try:
        import Core.post as post
    except Exception as ex:
        print("[!!!] [MicroPython Core] Core.post failed to import...")
        print(ex)
        return

    try:
        if post.script():
            import Core.initialization as init
            init.start("Startup")
        else:
            print("[!!!] [POST] Post check FAILED!")
    except Exception as ex:
        print("[!!!] [MicroPython Core] Core.initialization failed to import...")
        print("[!!!] [MicroPython Core] Or a critical error has occurred.")
        print(ex)

if __name__ == "__main__":
    while True:
        try:
            main()
            break  # clean exit from main()
        except KeyboardInterrupt:
            # USB CDC disconnect (or unhandled Ctrl+C) — restart OS silently.
            pass
        except Exception as ex:
            print("[!!!] Unhandled crash:", ex)
            import utime as _ut
            _ut.sleep(2)
            break