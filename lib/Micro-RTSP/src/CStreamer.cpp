#include "CStreamer.h"

#include <stdio.h>

CStreamer::CStreamer(SOCKET aClient, u_short width, u_short height) : m_Client(aClient)
{
    printf("Creating TSP streamer\n");
    m_RtpServerPort  = 0;
    m_RtcpServerPort = 0;
    m_RtpClientPort  = 0;
    m_RtcpClientPort = 0;

    m_SequenceNumber = 0;
    m_Timestamp      = 0;
    m_SendIdx        = 0;
    m_TCPTransport   = false;

    m_RtpSocket = NULLSOCKET;
    m_RtcpSocket = NULLSOCKET;

    m_width = width;
    m_height = height;
    m_prevMsec = 0;
};

CStreamer::~CStreamer()
{
    udpsocketclose(m_RtpSocket);
    udpsocketclose(m_RtcpSocket);
};

int CStreamer::SendRtpPacket(unsigned const char * jpeg, int jpegLen, int fragmentOffset, BufPtr quant0tbl, BufPtr quant1tbl)
{
#define KRtpHeaderSize 12           // size of the RTP header
#define KJpegHeaderSize 8           // size of the special JPEG payload header

#define MAX_FRAGMENT_SIZE 1100 // FIXME, pick more carefully
    int fragmentLen = MAX_FRAGMENT_SIZE;
    if(fragmentLen + fragmentOffset > jpegLen) // Shrink last fragment if needed
        fragmentLen = jpegLen - fragmentOffset;

    bool isLastFragment = (fragmentOffset + fragmentLen) == jpegLen;

    // Do we have custom quant tables? If so include them per RFC

    bool includeQuantTbl = quant0tbl && quant1tbl && fragmentOffset == 0;
    uint8_t q = includeQuantTbl ? 128 : 0x5e;

    static char RtpBuf[2048]; // Note: we assume single threaded, this large buf we keep off of the tiny stack
    int RtpPacketSize = fragmentLen + KRtpHeaderSize + KJpegHeaderSize + (includeQuantTbl ? (4 + 64 * 2) : 0);

    memset(RtpBuf,0x00,sizeof(RtpBuf));
    // Prepare the first 4 byte of the packet. This is the Rtp over Rtsp header in case of TCP based transport
    RtpBuf[0]  = '$';        // magic number
    RtpBuf[1]  = 0;          // number of multiplexed subchannel on RTPS connection - here the RTP channel
    RtpBuf[2]  = (RtpPacketSize & 0x0000FF00) >> 8;
    RtpBuf[3]  = (RtpPacketSize & 0x000000FF);
    // Prepare the 12 byte RTP header
    RtpBuf[4]  = 0x80;                               // RTP version
    RtpBuf[5]  = 0x1a | (isLastFragment ? 0x80 : 0x00);                               // JPEG payload (26) and marker bit
    RtpBuf[7]  = m_SequenceNumber & 0x0FF;           // each packet is counted with a sequence counter
    RtpBuf[6]  = m_SequenceNumber >> 8;
    RtpBuf[8]  = (m_Timestamp & 0xFF000000) >> 24;   // each image gets a timestamp
    RtpBuf[9]  = (m_Timestamp & 0x00FF0000) >> 16;
    RtpBuf[10] = (m_Timestamp & 0x0000FF00) >> 8;
    RtpBuf[11] = (m_Timestamp & 0x000000FF);
    RtpBuf[12] = 0x13;                               // 4 byte SSRC (sychronization source identifier)
    RtpBuf[13] = 0xf9;                               // we just an arbitrary number here to keep it simple
    RtpBuf[14] = 0x7e;
    RtpBuf[15] = 0x67;

    // Prepare the 8 byte payload JPEG header
    RtpBuf[16] = 0x00;                               // type specific
    RtpBuf[17] = (fragmentOffset & 0x00FF0000) >> 16;                               // 3 byte fragmentation offset for fragmented images
    RtpBuf[18] = (fragmentOffset & 0x0000FF00) >> 8;
    RtpBuf[19] = (fragmentOffset & 0x000000FF);

    /*    These sampling factors indicate that the chrominance components of
       type 0 video is downsampled horizontally by 2 (often called 4:2:2)
       while the chrominance components of type 1 video are downsampled both
       horizontally and vertically by 2 (often called 4:2:0). */
    RtpBuf[20] = 0x00;                               // type (fixme might be wrong for camera data) https://tools.ietf.org/html/rfc2435
    RtpBuf[21] = q;                               // quality scale factor was 0x5e
    RtpBuf[22] = m_width / 8;                           // width  / 8
    RtpBuf[23] = m_height / 8;                           // height / 8

    int headerLen = 24; // Inlcuding jpeg header but not qant table header
    if(includeQuantTbl) { // we need a quant header - but only in first packet of the frame
        //printf("inserting quanttbl\n");
        RtpBuf[24] = 0; // MBZ
        RtpBuf[25] = 0; // 8 bit precision
        RtpBuf[26] = 0; // MSB of lentgh

        int numQantBytes = 64; // Two 64 byte tables
        RtpBuf[27] = 2 * numQantBytes; // LSB of length

        headerLen += 4;

        memcpy(RtpBuf + headerLen, quant0tbl, numQantBytes);
        headerLen += numQantBytes;

        memcpy(RtpBuf + headerLen, quant1tbl, numQantBytes);
        headerLen += numQantBytes;
    }
    // printf("Sending timestamp %d, seq %d, fragoff %d, fraglen %d, jpegLen %d\n", m_Timestamp, m_SequenceNumber, fragmentOffset, fragmentLen, jpegLen);

    // append the JPEG scan data to the RTP buffer
    memcpy(RtpBuf + headerLen,jpeg + fragmentOffset, fragmentLen);
    fragmentOffset += fragmentLen;

    m_SequenceNumber++;                              // prepare the packet counter for the next packet

    IPADDRESS otherip;
    IPPORT otherport;
    socketpeeraddr(m_Client, &otherip, &otherport);

    // RTP marker bit must be set on last fragment
    if (m_TCPTransport) // RTP over RTSP - we send the buffer + 4 byte additional header
        socketsend(m_Client,RtpBuf,RtpPacketSize + 4);
    else                // UDP - we send just the buffer by skipping the 4 byte RTP over RTSP header
        udpsocketsend(m_RtpSocket,&RtpBuf[4],RtpPacketSize, otherip, m_RtpClientPort);

    return isLastFragment ? 0 : fragmentOffset;
};

void CStreamer::InitTransport(u_short aRtpPort, u_short aRtcpPort, bool TCP)
{
    m_RtpClientPort  = aRtpPort;
    m_RtcpClientPort = aRtcpPort;
    m_TCPTransport   = TCP;

    if (!m_TCPTransport)
    {   // allocate port pairs for RTP/RTCP ports in UDP transport mode
        for (u_short P = 6970; P < 0xFFFE; P += 2)
        {
            m_RtpSocket     = udpsocketcreate(P);
            if (m_RtpSocket)
            {   // Rtp socket was bound successfully. Lets try to bind the consecutive Rtsp socket
                m_RtcpSocket = udpsocketcreate(P + 1);
                if (m_RtcpSocket)
                {
                    m_RtpServerPort  = P;
                    m_RtcpServerPort = P+1;
                    break;
                }
                else
                {
                    udpsocketclose(m_RtpSocket);
                    udpsocketclose(m_RtcpSocket);
                };
            }
        };
    };
};

u_short CStreamer::GetRtpServerPort()
{
    return m_RtpServerPort;
};

u_short CStreamer::GetRtcpServerPort()
{
    return m_RtcpServerPort;
};

void CStreamer::streamFrame(unsigned const char *data, uint32_t dataLen, uint32_t curMsec)
{
    if(m_prevMsec == 0) // first frame init our timestamp
        m_prevMsec = curMsec;

    // compute deltat (being careful to handle clock rollover with a little lie)
    uint32_t deltams = (curMsec >= m_prevMsec) ? curMsec - m_prevMsec : 100;
    m_prevMsec = curMsec;

    // locate quant tables if possible
    BufPtr qtable0, qtable1;

    if(!decodeJPEGfile(&data, &dataLen, &qtable0, &qtable1)) {
        printf("can't decode jpeg data\n");
        return;
    }

    int offset = 0;
    do {
        offset = SendRtpPacket(data, dataLen, offset, qtable0, qtable1);
    } while(offset != 0);

    // Increment ONLY after a full frame
    uint32_t units = 90000; // Hz per RFC 2435
    m_Timestamp += (units * deltams / 1000);                             // fixed timestamp increment for a frame rate of 25fps

    m_SendIdx++;
    if (m_SendIdx > 1) m_SendIdx = 0;
};

#include <assert.h>

// search for a particular JPEG marker, moves *start to just after that marker
// This function fixes up the provided start ptr to point to the
// actual JPEG stream data and returns the number of bytes skipped
// APP0 e0
// DQT db
// DQT db
// DHT c4
// DHT c4
// DHT c4
// DHT c4
// SOF0 c0 baseline (not progressive) 3 color 0x01 Y, 0x21 2h1v, 0x00 tbl0
// - 0x02 Cb, 0x11 1h1v, 0x01 tbl1 - 0x03 Cr, 0x11 1h1v, 0x01 tbl1
// therefore 4:2:2, with two separate quant tables (0 and 1)
// SOS da
// EOI d9 (no need to strip data after this RFC says client will discard)
bool findJPEGheader(BufPtr *start, uint32_t *len, uint8_t marker) {
    // per https://en.wikipedia.org/wiki/JPEG_File_Interchange_Format
    unsigned const char *bytes = *start;

    // kinda skanky, will break if unlucky and the headers inxlucde 0xffda
    // might fall off array if jpeg is invalid
    // FIXME - return false instead
    while(bytes - *start < *len) {
        uint8_t framing = *bytes++; // better be 0xff
        if(framing != 0xff) {
            printf("malformed jpeg, framing=%x\n", framing);
            return false;
        }
        uint8_t typecode = *bytes++;
        if(typecode == marker) {
            unsigned skipped = bytes - *start;
            //printf("found marker 0x%x, skipped %d\n", marker, skipped);

            *start = bytes;

            // shrink len for the bytes we just skipped
            *len -= skipped;

            return true;
        }
        else {
            // not the section we were looking for, skip the entire section
            switch(typecode) {
            case 0xd8:     // start of image
            {
                break;   // no data to skip
            }
            case 0xe0:   // app0
            case 0xdb:   // dqt
            case 0xc4:   // dht
            case 0xc0:   // sof0
            case 0xda:   // sos
            {
                // standard format section with 2 bytes for len.  skip that many bytes
                // CLAUDE.md §4, §8: guard against untrusted length causing OOB read
                if ((bytes + 1) >= (*start + *len)) {
                    printf("malformed jpeg: section length field truncated\n");
                    return false;
                }
                uint32_t section_len = bytes[0] * 256 + bytes[1];
                //printf("skipping section 0x%x, %d bytes\n", typecode, section_len);
                if ((bytes + section_len) > (*start + *len)) {
                    printf("malformed jpeg: section length %u exceeds buffer\n", section_len);
                    return false;
                }
                bytes += section_len;
                break;
            }
            default:
                printf("unexpected jpeg typecode 0x%x\n", typecode);
                break;
            }
        }
    }

    printf("failed to find jpeg marker 0x%x", marker);
    return false;
}

#ifndef SKIP_SCAN_MAX_ITER
#define SKIP_SCAN_MAX_ITER   1024
#endif

// Bounded JPEG marker scanner. Scans for 0xFF followed by a non-zero byte
// (a valid JPEG marker). Advances *pos to the position of the 0xFF byte.
//
// CLAUDE.md §4, §8: every parsing loop over external/untrusted data must have
// an explicit max-iteration bound and a defined failure return path.
//
// Returns 0 if a valid marker (0xFF followed by non-zero) was found, -1 otherwise.
// *pos is updated to the last position examined on both success and failure.
//
// The scan is bounded by min(buf_len, *pos + max_iter); buf_len is ALWAYS the
// hard safety limit — the loop can never read past the end of the buffer no
// matter what max_iter is. Callers that need to locate the JPEG end-of-image
// marker MUST pass max_iter >= the scan-buffer length (i.e. buf_len): the EOI
// of a full-resolution frame lies tens of KB into the entropy data, so a small
// fixed cap (the old SKIP_SCAN_MAX_ITER = 1024) would reject valid frames.
int skipScanBytes(const uint8_t *buf, size_t buf_len, size_t *pos, size_t max_iter) {
    if (*pos >= buf_len) return -1;

    size_t i = *pos;
    size_t end = buf_len;
    if (i + max_iter < end) end = i + max_iter;

    while (i < end) {
        if (buf[i] == 0xFF) {
            if ((i + 1) < buf_len && buf[i + 1] != 0) {
                *pos = i;
                return 0; // found marker at *pos
            }
        }
        i++;
    }
    *pos = i;
    return -1; // limit exceeded or end of buffer
}
void  nextJpegBlock(BufPtr *bytes) {
    uint32_t len = (*bytes)[0] * 256 + (*bytes)[1];
    //printf("going to next jpeg block %d bytes\n", len);
    *bytes += len;
}

// When JPEG is stored as a file it is wrapped in a container
// This function fixes up the provided start ptr to point to the
// actual JPEG stream data and returns the number of bytes skipped
bool decodeJPEGfile(BufPtr *start, uint32_t *len, BufPtr *qtable0, BufPtr *qtable1) {
    // per https://en.wikipedia.org/wiki/JPEG_File_Interchange_Format
    unsigned const char *bytes = *start;

    // Remember the physical buffer origin/length up front (caller's fb->buf /
    // fb->len). The initial SOI search below (findJPEGheader ... 0xd8) decrements
    // *len by 2 WITHOUT advancing *start, so from that point on *len undercounts
    // the true bytes remaining from *start by exactly 2. We must therefore bound
    // the end-of-image scan by the TRUE remaining length computed from these,
    // not by *len (see the skipScanBytes call below).
    const unsigned char *buf_origin = *start;
    const uint32_t buf_total = *len;

    if(!findJPEGheader(&bytes, len, 0xd8)) // better at least look like a jpeg file
        return false; // FAILED!

    // Look for quant tables if they are present
    *qtable0 = NULL;
    *qtable1 = NULL;
    BufPtr quantstart = *start;
    uint32_t quantlen = *len;
    if(!findJPEGheader(&quantstart, &quantlen, 0xdb)) {
        printf("error can't find quant table 0\n");
    }
    else {
        // printf("found quant table %x\n", quantstart[2]);

        *qtable0 = quantstart + 3;     // 3 bytes of header skipped
        nextJpegBlock(&quantstart);
        if(!findJPEGheader(&quantstart, &quantlen, 0xdb)) {
            printf("error can't find quant table 1\n");
        }
        else {
            // printf("found quant table %x\n", quantstart[2]);
        }
        *qtable1 = quantstart + 3;
        nextJpegBlock(&quantstart);
    }

    if(!findJPEGheader(start, len, 0xda))
        return false; // FAILED!

    // Skip the header bytes of the SOS marker FIXME why doesn't this work?
    uint32_t soslen = (*start)[0] * 256 + (*start)[1];
    // Bounds-guard the SOS-segment skip (CLAUDE.md §8): reject if the declared
    // SOS length would push us to/past the end of the buffer, both to avoid
    // forming an out-of-range pointer and to guarantee there is entropy + EOI
    // data left to scan.
    if ((uint32_t)(*start - buf_origin) + soslen >= buf_total) {
        printf("malformed JPEG: SOS segment length %u exceeds buffer\n", soslen);
        return false; // FAILED!
    }
    *start += soslen;
    *len -= soslen;

    // start scanning the entropy-coded scan data for the end-of-image marker
    // (EOI, 0xFFD9).
    //
    // ROOT-CAUSE FIX (RTSP streaming was 100% broken): the "hardening" pass
    // capped this scan at SKIP_SCAN_MAX_ITER = 1024 bytes. A real OV2640 JPEG
    // has tens of KB of entropy-coded scan data before the EOI, and inside that
    // data every 0xFF is followed by a 0x00 stuffing byte — so no marker appears
    // until the very end. Capping at 1024 made skipScanBytes give up long before
    // the EOI, so decodeJPEGfile() failed on EVERY full-resolution frame
    // ("skipScanBytes did not find marker within bounds").
    //
    // The correct, SAFE bound is the number of bytes physically remaining from
    // *start to the end of the JPEG buffer — NOT *len. Because the initial SOI
    // search decremented *len by 2 without advancing *start, *len is exactly 2
    // bytes short here, and the EOI's 0xFF sits at relative index *len — one
    // past what a *len-bounded scan would examine (proven by adversarial trace).
    // buf_total/buf_origin come from the caller's fb->len/fb->buf, so scan_avail
    // is an explicit bound derived from the buffer's known size (CLAUDE.md §4/§8):
    // bounded (no unbounded loop) yet large enough to reach the EOI for any frame
    // size. The original geeksville code used an unbounded while(true), which
    // tolerated the 2-byte undercount by reading past *len into still-valid
    // memory; we keep a hard bound instead.
    uint32_t scan_avail = buf_total - (uint32_t)(*start - buf_origin);

    size_t scan_pos = 0;
    if (skipScanBytes(*start, scan_avail, &scan_pos, scan_avail) == -1) {
        printf("malformed JPEG: skipScanBytes did not find marker within bounds\n");
        return false; // FAILED!
    }

#ifdef STREAM_DEBUG
    // TEMPORARY diagnostic — remove with the -DSTREAM_DEBUG flag once verified.
    {
        static uint32_t sd_scan = 0;
        if (sd_scan < 10) {
            printf("STREAM_DEBUG skipScanBytes: marker at scan_pos=%u of scan_avail=%u (bytes 0x%02x 0x%02x)\n",
                   (unsigned)scan_pos, (unsigned)scan_avail,
                   (*start)[scan_pos],
                   (scan_pos + 1 < scan_avail) ? (*start)[scan_pos + 1] : 0);
            sd_scan++;
        }
    }
#endif

    BufPtr endmarkerptr = *start + scan_pos;
    uint32_t endlen = scan_avail - scan_pos;

    if(!findJPEGheader(&endmarkerptr, &endlen, 0xd9))
        return false; // FAILED!

    // endlen must now be the # of bytes between the start of our scan and
    // the end marker, tell the caller to ignore bytes afterwards
    *len = endmarkerptr - *start;

    return true;
}
