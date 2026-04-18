project('adm')

install_data(
    'group', 'localtime', 'passwd', 'profile', 'rc', 'shadow',
    install_dir: get_option('sysconfdir')
)

meson.add_install_script(
  'sh', '-c',
  '''
  ROOT="${DESTDIR}${MESON_INSTALL_PREFIX}"
  mkdir -p "$ROOT/dev"

  mkdir -p "$ROOT/tmp"
  chmod 1777 "$ROOT/tmp"

  mkdir -p "$ROOT/var/db"
  mkdir -p "$ROOT/var/run"

  ln -sf usr/bin  "$ROOT/bin"
  ln -sf usr/sbin "$ROOT/sbin"
  ln -sf var/run  "$ROOT/run"
  '''
)
