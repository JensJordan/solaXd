#!/usr/bin/env sh

if [ "$(id -u)" -eq 0 ]
then
  service solaxd stop
  systemctl disable solaxd
  rm /usr/bin/solaxd
  rm /lib/systemd/system/solaxd.service
  rm /etc/default/solaxd
  rm /etc/logrotate.d/solaxd

  echo "SolaXd is uninstalled, removing the uninstaller in progress ..."
  rm /usr/bin/uninstall-solaxd.sh
else
  echo "You need to be ROOT (sudo can be used)."
fi
