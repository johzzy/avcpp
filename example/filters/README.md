brew install spdlog
brew install llvm
ln -s "$(brew --prefix llvm)/bin/clang-format" "/usr/local/bin/clang-format"
ln -s "$(brew --prefix llvm)/bin/clang-tidy" "/usr/local/bin/clang-tidy"
ln -s "$(brew --prefix llvm)/bin/clang-apply-replacements" "/usr/local/bin/clang-apply-replacements"

混音模块设计

1. 混音处理

AMixContext

InputContext
OutputContext

OpusOutputContext

RtpInputContext

```
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=60;useinbandfec=1;maxaveragebitrate=24000;usedtx=1;stereo=0;sprop-stereo=0
```

2. 输入端

```
读取 rtp packet

int InputProcessor::deliverAudioData_(std::shared_ptr<DataPacket> audio_packet)
int InputProcessor::decodeAudio(unsigned char* inBuff, int inBuffLen, unsigned char* outBuff)
int InputProcessor::unpackageAudio(unsigned char* inBuff, int inBuffLen, unsigned char* outBuff) {
  int l = inBuffLen - RtpHeader::MIN_SIZE;
  if (l < 0) {
    ELOG_ERROR("Error unpackaging audio");
    return 0;
  }
  memcpy(outBuff, &inBuff[RtpHeader::MIN_SIZE], l);

  return l;
}
```


3. 输出端

```
合成rtp packet
int OutputProcessor::packageAudio(unsigned char* inBuff, int inBuffLen, unsigned char* outBuff,
                                  long int pts) {  // NOLINT
  if (audioPackager == 0) {
    ELOG_DEBUG("No se ha inicializado el codec de output audio RTP");
    return -1;
  }

  // uint64_t millis = ClockUtils::timePointToMs(clock::now());

  RtpHeader head;
  head.setSeqNumber(audioSeqnum_++);
  // head.setTimestamp(millis*8);
  head.setMarker(1);
  if (pts == 0) {
    // head.setTimestamp(audioSeqnum_*160);
    head.setTimestamp(av_rescale(audioSeqnum_, (mediaInfo.audioCodec.sampleRate/1000), 1));
  } else {
    // head.setTimestamp(pts*8);
    head.setTimestamp(av_rescale(pts, mediaInfo.audioCodec.sampleRate, 1000));
  }
  head.setSSRC(44444);
  head.setPayloadType(mediaInfo.rtpAudioInfo.PT);

  // memcpy (rtpAudioBuffer_, &head, head.getHeaderLength());
  // memcpy(&rtpAudioBuffer_[head.getHeaderLength()], inBuff, inBuffLen);
  memcpy(outBuff, &head, head.getHeaderLength());
  memcpy(&outBuff[head.getHeaderLength()], inBuff, inBuffLen);
  // sink_->sendData(rtpBuffer_, l);
  // rtpReceiver_->receiveRtpData(rtpBuffer_, (inBuffLen + RTP_HEADER_LEN));
  return (inBuffLen + head.getHeaderLength());
}
```
