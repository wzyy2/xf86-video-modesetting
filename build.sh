make clean
make
if [ $? -eq 0 ]       
then  
	make install
	mv -f /usr/local/lib/xorg/modules/drivers/* /usr/lib/xorg/modules/drivers/
#	service lightdm restart
fi
