all: x509_crt_bundle

clean:
	[ -f x509_crt_bundle ] && rm -f x509_crt_bundle

cacert.pem:
	if [ -f cacert.pem ]; then\
		curl --remote-name --time-cond cacert.pem https://curl.se/ca/cacert.pem ;\
	else\
		curl --remote-name https://curl.se/ca/cacert.pem ;\
	fi

x509_crt_bundle: cacert.pem gen_crt_bundle.py
	python gen_crt_bundle.py -i cacert.pem

