main: xdg-shell-protocol.h main.c
	$(CC) -o main -Wall -Wextra -Wpedantic -I/usr/include/wlroots-0.18 -I/usr/include/pixman-1 -I. -DWLR_USE_UNSTABLE main.c -lwayland-server -lwlroots-0.18

xdg-shell-protocol.h:
	wayland-scanner server-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml $@
