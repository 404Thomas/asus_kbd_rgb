# asus_kbd_rgb

🌐 **Português** · [English](README.md)

Controlador de iluminação RGB para o teclado de notebooks **ASUS VivoBook S14
(S5406)** no Linux, escrito em C puro.

O backlight desse modelo é de **zona única** (todas as teclas mudam de cor ao
mesmo tempo) e exposto como um dispositivo HID `LampArray`
(`ITE5570`, VID `0x0B05` / PID `0x19B6`). Este programa fala diretamente com o
controlador via `hidraw`, sem depender de drivers proprietários, e implementa
efeitos animados (breathe, cycle) por software usando `pthreads`.

## Recursos

- 🎨 **Cor estática** em qualquer valor hexadecimal (`RRGGBB`)
- 🌬️ **Breathe** — pulsa suavemente a cor escolhida (curva senoidal)
- 🌈 **Cycle** — percorre todo o espectro HSV
- 🔆 **4 níveis de brilho** e 3 velocidades (slow / med / fast)
- 💾 **Persistência**: o último estado é salvo e restaurado no boot e ao
  acordar da suspensão (via systemd)
- 🧰 **Menu interativo** no terminal
- 🔌 Roda **sem root** com uma regra udev
- 🪶 Sem dependências além de `libc`, `libm` e `pthread`

## Compatibilidade

| Item        | Valor                                   |
|-------------|-----------------------------------------|
| Notebook    | ASUS VivoBook S14 S5406                  |
| Controlador | ITE5570 (HID LampArray, zona única)     |
| USB IDs     | VID `0x0B05`, PID `0x19B6` (e `0x1B2C`)  |
| SO          | Linux (testado em Fedora)               |

> Outros modelos ASUS com o mesmo controlador ITE de zona única podem
> funcionar. Use `--debug` para conferir VID/PID do seu hardware.

## Instalação

### Requisitos

```sh
# Fedora
sudo dnf install gcc make

# Debian/Ubuntu
sudo apt install build-essential
```

### Compilar e instalar

```sh
git clone https://github.com/<seu-usuario>/asus_kbd_rgb.git
cd asus_kbd_rgb

make                 # compila o binário ./asus_kbd_rgb
sudo make install    # instala em /usr/local/bin
```

### Acesso sem root (recomendado)

Por padrão o `/dev/hidraw*` exige root. A regra udev libera o acesso para o
grupo `input`:

```sh
sudo make udev
sudo usermod -aG input $USER
# faça logout e login para o grupo entrar em vigor
```

### Restaurar a cor no boot (opcional)

Instala um serviço systemd que reaplica o último estado salvo ao ligar o
notebook e ao retornar da suspensão/hibernação:

```sh
sudo make service
```

## Uso

```sh
asus_kbd_rgb [opções]
```

| Opção                | Descrição                                              |
|----------------------|--------------------------------------------------------|
| `--color <cor>`      | Cor hexadecimal: `FF0000` ou `"#FF0000"`               |
| `--mode <modo>`      | `static` \| `breathe` \| `cycle` \| `off`              |
| `--speed <vel>`      | `slow` \| `med` \| `fast` (para breathe e cycle)       |
| `--brightness <0-3>` | Nível de brilho                                        |
| `--on`               | Liga com branco no brilho máximo                       |
| `--off`              | Desliga o backlight                                    |
| `--restore`          | Reaplica o último estado salvo (usado no boot)         |
| `--foreground`       | Mantém breathe/cycle no terminal (não vira daemon)     |
| `--interactive`      | Abre o menu interativo                                 |
| `--debug`            | Mostra informações do hardware                         |
| `--help`             | Ajuda                                                  |

### Exemplos

```sh
# Cor estática verde-água no brilho máximo
asus_kbd_rgb --color 00FF88 --brightness 3

# Efeito breathe laranja, lento
asus_kbd_rgb --mode breathe --color FF6600 --speed slow

# Espectro completo, rápido
asus_kbd_rgb --mode cycle --speed fast

# Desligar / menu interativo
asus_kbd_rgb --off
asus_kbd_rgb --interactive
```

> **Animações em segundo plano:** `breathe` e `cycle` precisam de um processo
> vivo para animar, então o programa vira *daemon* e libera o terminal. Use
> `--off` para pará-las (ou `--foreground` para mantê-las no terminal até
> `Ctrl+C`).

## Como funciona

- **HID / hidraw** — o controlador é localizado automaticamente varrendo
  `/sys/class/hidraw/*` em busca do VID/PID conhecido. As cores são enviadas
  por *feature reports* do protocolo HID LampArray (`LampArrayControl` para
  assumir o controle do host e `LampRangeUpdate` para pintar todas as lâmpadas
  de uma vez).
- **Efeitos** — como o firmware só faz cor fixa, breathe e cycle são gerados
  em uma thread que recalcula a cor a cada frame (`breathe` por curva senoidal,
  `cycle` percorrendo o matiz HSV). Cor, brilho e velocidade podem ser
  ajustados em tempo real pelo menu interativo.
- **Persistência** — o estado atual é gravado em
  `/var/lib/asus_kbd_rgb/state` (sobrescrevível via `ASUS_KBD_RGB_STATE`).
  `--restore` o relê; o serviço systemd chama `--restore` no boot e ao acordar.
- **Daemon único** — o PID do daemon de animação fica em
  `/var/lib/asus_kbd_rgb/pid`, garantindo que apenas um efeito rode por vez.

## Estrutura do projeto

```
asus_kbd_rgb/
├── src/
│   └── asus_kbd_rgb.c          # código-fonte (arquivo único)
├── systemd/
│   └── asus-kbd-rgb.service    # serviço de restauração no boot
├── udev/
│   └── 99-asus-kbd-rgb.rules   # regra de acesso sem root
├── Makefile
├── LICENSE
└── README.md
```

## Desinstalar

```sh
sudo make uninstall
```

## Aviso

Projeto independente, sem qualquer vínculo com a ASUS. Use por sua conta e
risco — ele envia comandos HID diretamente ao controlador do teclado.

## Licença

[MIT](LICENSE)
