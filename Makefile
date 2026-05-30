CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lpthread -lm

TARGET  = asus_kbd_rgb
SRC     = src/asus_kbd_rgb.c

PREFIX     ?= /usr/local
BINDIR      = $(PREFIX)/bin
UDEV_RULE   = udev/99-asus-kbd-rgb.rules
SERVICE     = systemd/asus-kbd-rgb.service

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

# Instala o binário em $(BINDIR)
install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Instalado em $(DESTDIR)$(BINDIR)/$(TARGET)"
	@echo "Para usar sem root:        sudo make udev"
	@echo "Para restaurar no boot:    sudo make service"

# Regra udev: acesso ao teclado sem root
udev:
	install -Dm644 $(UDEV_RULE) /etc/udev/rules.d/99-asus-kbd-rgb.rules
	udevadm control --reload
	udevadm trigger
	@echo "Regra udev instalada."
	@echo "Adicione seu usuário ao grupo: sudo usermod -aG input $$USER"
	@echo "Depois faça logout e login."

# Serviço systemd: restaura a cor/efeito no boot e ao acordar da suspensão
service: install
	install -Dm644 $(SERVICE) /etc/systemd/system/asus-kbd-rgb.service
	systemctl daemon-reload
	systemctl enable asus-kbd-rgb.service
	@echo "Serviço habilitado: a última cor/efeito será restaurada no boot."

uninstall:
	-systemctl disable asus-kbd-rgb.service 2>/dev/null
	rm -f /etc/systemd/system/asus-kbd-rgb.service
	systemctl daemon-reload
	rm -f $(BINDIR)/$(TARGET)
	rm -f /etc/udev/rules.d/99-asus-kbd-rgb.rules

clean:
	rm -f $(TARGET) *.o

.PHONY: all install udev service uninstall clean
