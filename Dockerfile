FROM nvcr.io/nvidia/l4t-base:r32.6.1 AS build
COPY ffmpeg /build/ffmpeg
COPY jetson-ffmpeg /build/jetson-ffmpeg
COPY jetson_multimedia_api /usr/src/jetson_multimedia_api
RUN apt-get update && apt-get -y --autoremove install build-essential git libass-dev cmake
WORKDIR /build/jetson-ffmpeg
RUN mkdir build && cd build && cmake .. && make -j4 && make install && ldconfig
WORKDIR /build/ffmpeg
RUN ./configure --enable-nvmpi --enable-libass && make -j4

FROM nvcr.io/nvidia/l4t-base:r32.6.1
ENV LD_LIBRARY_PATH=/usr/lib:$LD_LIBRARY_PATH
COPY --from=build /usr/local/lib/libnvmpi.a /usr/lib
COPY --from=build /usr/local/lib/libnvmpi.so.1.0.0 /usr/lib
COPY --from=build /build/ffmpeg/ffmpeg /usr/bin
COPY --from=build /build/ffmpeg/ffprobe /usr/bin
RUN ln /usr/lib/libnvmpi.so.1.0.0 /usr/lib/libnvmpi.so

CMD [ "/bin/bash" ]
