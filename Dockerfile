FROM python:latest
MAINTAINER Alex Tarasov "alxndrtarasov@gmail.com"
RUN apt-get update -y
#RUN apt-get install -y pip python-dev build-essential
COPY . ./iroha-py
WORKDIR ./iroha-py
RUN pip3 install flask
ENTRYPOINT ["python"]
CMD ["app.py"]