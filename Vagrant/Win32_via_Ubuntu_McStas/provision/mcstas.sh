#! /bin/bash

cd /home/vagrant
sudo -u vagrant git clone https://github.com/McStasMcXtrace/McCode.git --recurse-submodules 

#sudo -u vagrant tar xzf McCode/support/Win32/Wine/dotwine.tgz

# Hack to allow calling wine without errors
#ln -s /home/vagrant/.wine /root/.wine
#chmod a+x /root

#rm /home/vagrant/.wine/dosdevices/z\:
#ln -s /home/vagrant /home/vagrant/.wine/dosdevices/z\:

cd McCode
git pull
./getdeps_win64
sudo -u vagrant ./build_windows_mcstas test meta

