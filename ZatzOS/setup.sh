#!/bin/bash
# =====================================================================
#  ZatzOS 2.0 — نظام خفيف وقوي فوق Debian (مجرّب لـ VirtualBox)
#
#  الاستعمال (بعد تنصيب Debian بلا واجهة):
#     su -
#     apt install -y wget
#     wget -O setup.sh https://raw.githubusercontent.com/lalryby420-blip/zatz-lab/main/ZatzOS/setup.sh
#     bash setup.sh اسم_اليوزر
#     reboot
# =====================================================================
set -e

OS_NAME="ZatzOS"
OS_VERSION="2.0"
OS_ID="zatzos"

U="$1"
if [ "$(id -u)" -ne 0 ]; then echo "لازم root: اكتب  su -  الأول"; exit 1; fi
if [ -z "$U" ] || ! id "$U" >/dev/null 2>&1; then
  echo "الاستعمال: bash setup.sh USERNAME   (اسم اليوزر اللي درته وقت التنصيب)"; exit 1
fi
H="/home/$U"

step() { echo; echo "==> [$1/9] $2"; }

# ---------------------------------------------------------------------
step 1 "إعداد apt (يعاود لو النت طاح)"
cat > /etc/apt/apt.conf.d/80-zatzos <<'EOF'
Acquire::Retries "10";
Acquire::http::Timeout "30";
APT::Install-Recommends "false";
EOF
apt-get update

# ---------------------------------------------------------------------
step 2 "تنصيب الواجهة والبرامج (حوالي 400-600 ميغا تحميل)"
PKGS="xserver-xorg xinit x11-xserver-utils x11-xkb-utils dbus-x11
openbox tint2 rofi feh dunst libnotify-bin lxpolkit
xfce4-terminal pcmanfm mousepad firefox-esr flameshot
network-manager network-manager-gnome
pipewire pipewire-pulse wireplumber pavucontrol
fonts-dejavu-core fonts-noto-core fonts-noto-color-emoji
sudo git curl wget unzip p7zip-full htop python3 python3-pip
build-essential xdg-user-dirs gvfs ca-certificates"
DEBIAN_FRONTEND=noninteractive apt-get install -y $PKGS
# fastfetch موجود في Debian 13، ولو مش موجود نكمّلوا عادي
DEBIAN_FRONTEND=noninteractive apt-get install -y fastfetch 2>/dev/null || true

# ---------------------------------------------------------------------
step 3 "اليوزر: صلاحيات sudo ودخول تلقائي"
usermod -aG sudo,netdev,audio,video "$U"
echo "$U ALL=(ALL) NOPASSWD: /usr/bin/systemctl poweroff, /usr/bin/systemctl reboot" > /etc/sudoers.d/zatzos-power
chmod 440 /etc/sudoers.d/zatzos-power

mkdir -p /etc/systemd/system/getty@tty1.service.d
cat > /etc/systemd/system/getty@tty1.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $U --noclear %I \$TERM
EOF

# NetworkManager يدير الشبكة (باش أيقونة الشبكة تخدم)
sed -i 's/^managed=false/managed=true/' /etc/NetworkManager/NetworkManager.conf 2>/dev/null || true

# ---------------------------------------------------------------------
step 4 "تشغيل الواجهة تلقائياً"
grep -q "ZatzOS: startx" "$H/.bash_profile" 2>/dev/null || cat >> "$H/.bash_profile" <<'EOF'

# ZatzOS: startx على tty1
[ -f ~/.bashrc ] && . ~/.bashrc
if [ -z "$DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then exec startx; fi
EOF

cat > "$H/.xinitrc" <<'EOF'
xrdb -merge ~/.Xresources 2>/dev/null
setxkbmap -layout us,ara -option grp:alt_shift_toggle
exec openbox-session
EOF

# ---------------------------------------------------------------------
step 5 "الخلفية"
mkdir -p "$H/.local/share/zatzos"
python3 - "$H/.local/share/zatzos/wall.ppm" <<'PY'
import sys
W, Hh = 1920, 1080
c1, c2 = (8, 14, 24), (10, 70, 90)
with open(sys.argv[1], "wb") as f:
    f.write(b"P6 %d %d 255\n" % (W, Hh))
    row = bytearray()
    for y in range(Hh):
        t = y / (Hh - 1)
        px = bytes(int(a + (b - a) * t) for a, b in zip(c1, c2))
        f.write(px * W)
PY

# ---------------------------------------------------------------------
step 6 "Openbox: تشغيل تلقائي + اختصارات + قائمة"
mkdir -p "$H/.config/openbox"
cat > "$H/.config/openbox/autostart" <<'EOF'
xset s off -dpms &
feh --bg-fill ~/.local/share/zatzos/wall.ppm &
tint2 &
nm-applet &
lxpolkit &
dunst &
(sleep 2; notify-send "ZatzOS 2.0" "Ctrl+Space: البرامج | Ctrl+Alt+T: تيرمينال | Alt+Shift: عربي/إنجليزي") &
xfce4-terminal &
EOF

K=''
add_key() { K+="<keybind key=\"$1\"><action name=\"Execute\"><command>$2</command></action></keybind>"; }
add_key "C-space" "rofi -show drun -show-icons"
add_key "A-F1"    "rofi -show drun -show-icons"
add_key "A-F2"    "rofi -show run"
add_key "C-A-t"   "xfce4-terminal"
add_key "C-A-b"   "firefox-esr"
add_key "C-A-f"   "pcmanfm"
add_key "C-A-e"   "mousepad"
add_key "Print"   "flameshot gui"
K+='<keybind key="C-A-q"><action name="Close"/></keybind>'
K+='<keybind key="A-Tab"><action name="NextWindow"/></keybind>'
K+='<keybind key="C-A-Up"><action name="ToggleMaximize"/></keybind>'
K+='<keybind key="C-A-Left"><action name="UnmaximizeFull"/><action name="MoveResizeTo"><x>0</x><y>0</y><width>50%</width><height>100%</height></action></keybind>'
K+='<keybind key="C-A-Right"><action name="UnmaximizeFull"/><action name="MoveResizeTo"><x>-0</x><y>0</y><width>50%</width><height>100%</height></action></keybind>'
sed "s#</keyboard>#${K}</keyboard>#" /etc/xdg/openbox/rc.xml > "$H/.config/openbox/rc.xml"

cat > "$H/.config/openbox/menu.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="res-menu" label="Resolution">
    <item label="1280x720"><action name="Execute"><command>zatz res 1280x720</command></action></item>
    <item label="1366x768"><action name="Execute"><command>zatz res 1366x768</command></action></item>
    <item label="1600x900"><action name="Execute"><command>zatz res 1600x900</command></action></item>
    <item label="1920x1080"><action name="Execute"><command>zatz res 1920x1080</command></action></item>
  </menu>
  <menu id="root-menu" label="$OS_NAME $OS_VERSION">
    <item label="Apps (Ctrl+Space)"><action name="Execute"><command>rofi -show drun -show-icons</command></action></item>
    <separator/>
    <item label="Terminal"><action name="Execute"><command>xfce4-terminal</command></action></item>
    <item label="Firefox"><action name="Execute"><command>firefox-esr</command></action></item>
    <item label="Files"><action name="Execute"><command>pcmanfm</command></action></item>
    <item label="Text Editor"><action name="Execute"><command>mousepad</command></action></item>
    <item label="Screenshot"><action name="Execute"><command>flameshot gui</command></action></item>
    <item label="Sound"><action name="Execute"><command>pavucontrol</command></action></item>
    <separator/>
    <menu id="res-menu"/>
    <item label="Update System"><action name="Execute"><command>xfce4-terminal -e "zatz update"</command></action></item>
    <separator/>
    <item label="Reboot"><action name="Execute"><command>sudo systemctl reboot</command></action></item>
    <item label="Shutdown"><action name="Execute"><command>sudo systemctl poweroff</command></action></item>
  </menu>
</openbox_menu>
EOF

# ---------------------------------------------------------------------
step 7 "اللوحة (tint2) والشكل"
mkdir -p "$H/.config/tint2"
cat > "$H/.config/tint2/tint2rc" <<'EOF'
# ZatzOS panel
rounded = 0
border_width = 0
background_color = #0b1220 92
border_color = #000000 0

rounded = 6
border_width = 0
background_color = #1d2b3f 100
border_color = #000000 0

panel_items = LTSC
panel_position = bottom center horizontal
panel_size = 100% 34
panel_margin = 0 0
panel_padding = 6 4 6
panel_background_id = 1
panel_layer = top
autohide = 0

launcher_padding = 4 2 6
launcher_background_id = 0
launcher_icon_size = 24
launcher_item_app = /usr/share/applications/xfce4-terminal.desktop
launcher_item_app = /usr/share/applications/firefox-esr.desktop
launcher_item_app = /usr/share/applications/pcmanfm.desktop
launcher_item_app = /usr/share/applications/org.xfce.mousepad.desktop

taskbar_mode = single_desktop
taskbar_padding = 2 2 4
taskbar_background_id = 0
task_text = 1
task_icon = 1
task_maximum_size = 200 30
task_padding = 6 3 6
task_font_color = #d6e2ee 100
task_background_id = 0
task_active_background_id = 2

systray_padding = 4 2 6
systray_background_id = 0
systray_icon_size = 20

time1_format = %H:%M
time2_format = %a %d %b
clock_font_color = #d6e2ee 100
clock_padding = 8 0
clock_background_id = 0
EOF

cat > "$H/.Xresources" <<'EOF'
Xft.dpi: 96
Xft.antialias: true
Xft.hinting: true
Xft.rgba: rgb
EOF

mkdir -p "$H/.config/xfce4/terminal"
cat > "$H/.config/xfce4/terminal/terminalrc" <<'EOF'
[Configuration]
FontName=DejaVu Sans Mono 11
ColorBackground=#0b1220
ColorForeground=#d6e2ee
MiscMenubarDefault=FALSE
MiscShowUnsafePasteDialog=FALSE
ScrollingLines=10000
EOF

mkdir -p "$H/.config/rofi"
cat > "$H/.config/rofi/config.rasi" <<'EOF'
configuration { modi: "drun,run"; show-icons: true; drun-display-format: "{name}"; }
* { background-color: #0b1220; text-color: #d6e2ee; font: "DejaVu Sans 12"; }
window { width: 40%; border: 2px; border-color: #5ad1e6; padding: 10px; }
element selected { background-color: #1d2b3f; text-color: #5ad1e6; }
inputbar { padding: 6px; }
EOF

# ---------------------------------------------------------------------
step 8 "أمر zatz (أداة النظام)"
cat > /usr/local/bin/zatz <<'EOF'
#!/bin/bash
# zatz — أداة ZatzOS
case "$1" in
  update)  sudo apt update && sudo apt full-upgrade -y; echo; read -rp "Done. Enter to close..." ;;
  install) shift; sudo apt install -y "$@" ;;
  remove)  shift; sudo apt remove -y "$@" ;;
  search)  shift; apt search "$@" 2>/dev/null | less ;;
  info)    command -v fastfetch >/dev/null && fastfetch || { . /etc/os-release; echo "$PRETTY_NAME | $(uname -r) | RAM: $(free -h | awk '/Mem/{print $3"/"$2}') | Disk: $(df -h / | awk 'NR==2{print $3"/"$2}')"; } ;;
  res)     xrandr -s "$2" 2>/dev/null || notify-send "zatz" "Resolution $2 not available (VirtualBox: Display > Graphics Controller = VMSVGA)" ;;
  ai-setup)
           echo "Installing Ollama (needs ~1-2 GB download for a model). Ctrl+C to cancel."; sleep 3
           curl -fsSL https://ollama.com/install.sh | sh
           echo "Now try:  ollama run <model-name>   (pick a small model, 1-3B)" ;;
  *)       cat <<H
zatz — ZatzOS tool
  zatz info              system info
  zatz update            update everything
  zatz install <pkg>     install a program
  zatz remove <pkg>      remove a program
  zatz search <word>     search programs
  zatz res 1366x768      change resolution
  zatz ai-setup          install Ollama (local AI) — big download
H
  ;;
esac
EOF
chmod +x /usr/local/bin/zatz

# ---------------------------------------------------------------------
step 9 "الهوية والإقلاع"
hostnamectl set-hostname "$OS_ID" 2>/dev/null || echo "$OS_ID" > /etc/hostname
sed -i "s/127.0.1.1.*/127.0.1.1\t$OS_ID/" /etc/hosts || true
rm -f /etc/os-release
cat > /etc/os-release <<EOF
PRETTY_NAME="$OS_NAME $OS_VERSION"
NAME="$OS_NAME"
VERSION="$OS_VERSION"
ID=$OS_ID
ID_LIKE=debian
HOME_URL="https://github.com/lalryby420-blip/zatz-lab"
EOF
echo "$OS_NAME $OS_VERSION \\n \\l" > /etc/issue

grep -q "ZatzOS prompt" "$H/.bashrc" || cat >> "$H/.bashrc" <<'EOF'

# ===== ZatzOS prompt =====
PS1='\[\e[1;36m\]zatz\[\e[0m\]@\[\e[1;35m\]\h\[\e[0m\]:\[\e[1;33m\]\w\[\e[0m\]\$ '
alias ll='ls -la --color=auto'
alias update='zatz update'
if [ -n "$DISPLAY" ] && [ -z "$ZATZ_SHOWN" ]; then
  export ZATZ_SHOWN=1
  zatz info 2>/dev/null
  echo "  type: zatz   for help"
fi
EOF

sed -i 's/^GRUB_TIMEOUT=.*/GRUB_TIMEOUT=1/' /etc/default/grub
sed -i "s/^GRUB_DISTRIBUTOR=.*/GRUB_DISTRIBUTOR=\"$OS_NAME\"/" /etc/default/grub
sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="quiet loglevel=3"/' /etc/default/grub
update-grub

sudo -u "$U" xdg-user-dirs-update 2>/dev/null || true
chown -R "$U:$U" "$H"

echo
echo "================================================"
echo "  $OS_NAME $OS_VERSION جاهز!  اكتب:  reboot"
echo "================================================"
