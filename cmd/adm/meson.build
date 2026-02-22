project('adm')

install_data(
    'group', 'localtime', 'passwd', 'profile', 'rc',
    install_dir: get_option('sysconfdir')
)

meson.add_install_script(
  'sh', '-c',
  '''
  ROOT="${DESTDIR}${MESON_INSTALL_PREFIX}"
  mkdir -p "$ROOT/dev"
  ln -sf usr/bin  "$ROOT/bin"
  ln -sf usr/sbin "$ROOT/sbin"
  '''
)
