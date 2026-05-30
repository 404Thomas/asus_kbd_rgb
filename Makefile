CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lpthread -lm
TARGET  = asus_kbd_rgb

all: $(TARGET)

$(TARGET): asus_kbd_rgb.c
	$(CC) $(CFLAGS) -o $(TARGET) asus_kbd_rgb.c $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Instalado em /usr/local/bin/$(TARGET)"
	@echo "Para usar sem root, execute: sudo make udev"

udev:
	@echo 'KERNEL=="hidraw*", ATTRS{idVendor}=="0b05", ATTRS{idProduct}=="19b6", MODE="0660", GROUP="input"' \
	  > /etc/udev/rules.d/99-asus-kbd-rgb.rules
	udevadm control --reload
	udevadm trigger
	@echo "Regra udev instalada."
	@echo "Adicione seu usuário ao grupo: sudo usermod -aG input $$USER"
	@echo "Depois faça logout e login."

uninstall:
	rm -f /usr/local/bin/$(TARGET)
	rm -f /etc/udev/rules.d/99-asus-kbd-rgb.rules

clean:
	rm -f $(TARGET)

.PHONY: all install udev uninstall clean
