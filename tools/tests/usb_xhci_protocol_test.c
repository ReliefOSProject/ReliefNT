/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

/* Construct one real Supported Protocol capability for the production parser. */
static int parse_usb(unsigned major, unsigned minor, const uint32_t *psi,
                     unsigned count, struct xhci_protocol *port)
{
    uint32_t regs[24]={0};
    regs[4]=(major<<24)|(minor<<16)|2u;
    regs[5]=0x20425355u;
    regs[6]=(count<<28)|(1u<<8)|1u;
    for (unsigned i=0;i<count;++i) regs[8+i]=psi[i];
    return xhci_protocol_parse(regs,sizeof(regs),16,1,port);
}

/* xHCI 1.2c Table 7-13 supplies the equivalent PSI values, including the
 * advertised 10 and 20 Gb/s values for the two USB 3.2 x2 defaults. */
static void implied_speeds(void)
{
    struct xhci_protocol p;
    assert(!parse_usb(3,0,NULL,0,&p));
    assert(p.valid_speeds==0x10 && p.speeds[4]==3 && p.rates[4]==5000000000ull);
    assert(!parse_usb(3,0x10,NULL,0,&p));
    assert(p.valid_speeds==0x30 && p.speeds[5]==4 && p.rates[5]==10000000000ull);
    assert(!parse_usb(3,0x20,NULL,0,&p));
    assert(p.valid_speeds==0xf0 && p.speeds[5]==4 && p.speeds[6]==4 && p.speeds[7]==4);
    assert(p.psi_count==0);
    assert(p.rates[5]==10000000000ull && p.rates[6]==10000000000ull &&
           p.rates[7]==20000000000ull);
    assert(!parse_usb(2,0,NULL,0,&p));
    assert(p.valid_speeds==0x0e && p.speeds[1]==0 && p.speeds[2]==1 && p.speeds[3]==2);
}

/* Explicit IDs are controller-defined, not the default speed IDs. */
static void explicit_speeds(void)
{
    struct xhci_protocol p;
    const uint32_t s31[]={ (5u<<16)|(1u<<8)|(3u<<4)|9u,
                           (10u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|3u };
    assert(!parse_usb(3,0x10,s31,2,&p));
    assert(p.valid_speeds==((1u<<9)|(1u<<3)) && p.speeds[9]==3 && p.speeds[3]==4);
    assert(p.rates[9]==5000000000ull && p.rates[3]==10000000000ull);
    assert(p.psi_count==2 && p.psi[0]==s31[0] && p.psi[1]==s31[1]);
    const uint32_t s32[]={ (10u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|8u,
                           (20u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|11u };
    assert(!parse_usb(3,0x20,s32,2,&p));
    assert(p.valid_speeds==((1u<<8)|(1u<<11)) && p.speeds[8]==4 && p.speeds[11]==4);
    assert(p.rates[8]==10000000000ull && p.rates[11]==20000000000ull);
    assert(p.psi_count==2 && p.psi[0]==s32[0] && p.psi[1]==s32[1]);
    const uint32_t alternate=(10000u<<16)|(1u<<14)|(1u<<8)|(2u<<4)|12u;
    assert(!parse_usb(3,0x20,&alternate,1,&p));
    assert(p.valid_speeds==(1u<<12) && p.rates[12]==10000000000ull && p.psi[0]==alternate);
}

/* Invalid revision/LP/PFD, reserved fields, and unsupported rates cannot
 * become usable merely because their PSIV looks like a known default. */
static void rejected_speeds(void)
{
    struct xhci_protocol p;
    uint32_t psi=(480u<<16)|(1u<<8)|(2u<<4)|3u;
    int rc=parse_usb(2,0,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<3))); /* USB2 is half-duplex */
    psi=(10u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|5u;
    rc=parse_usb(3,0,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<5))); /* 3.0 has no SSP */
    rc=parse_usb(3,0x30,&psi,1,&p);
    assert(rc || !p.valid_speeds); /* unknown protocol revision */
    rc=parse_usb(2,0x10,&psi,1,&p);
    assert(rc || !p.valid_speeds);
    psi=(10u<<16)|(1u<<14)|(3u<<4)|5u;
    rc=parse_usb(3,0x10,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<5))); /* missing full duplex */
    psi=(10u<<16)|(2u<<14)|(1u<<8)|(3u<<4)|5u;
    rc=parse_usb(3,0x10,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<5))); /* reserved LP */
    psi=(10u<<16)|(3u<<14)|(1u<<8)|(3u<<4)|5u;
    rc=parse_usb(3,0x10,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<5))); /* other reserved LP */
    psi=(10u<<16)|(1u<<14)|(1u<<8)|(1u<<6)|(3u<<4)|5u;
    assert(parse_usb(3,0x10,&psi,1,&p)==-EINVAL); /* reserved PLT */
    psi=(11u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|5u;
    rc=parse_usb(3,0x20,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<5))); /* unsupported rate */
    psi=(20u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|5u;
    rc=parse_usb(3,0x10,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<5))); /* 20 Gb/s needs USB 3.2 */
    psi=(10u<<16)|(1u<<8)|(3u<<4)|5u;
    rc=parse_usb(3,0x20,&psi,1,&p);
    assert(rc || !(p.valid_speeds&(1u<<5))); /* 10 Gb/s is not LP 0 */
}

/* RsvdP bits are ignored for evaluation but retained in the raw dword. */
static void noisy_symmetric(void)
{
    struct xhci_protocol p;
    uint32_t psi=(10u<<16)|(1u<<14)|(0x15u<<9)|(1u<<8)|(3u<<4)|13u;
    assert(!parse_usb(3,0x10,&psi,1,&p));
    assert(p.valid_speeds==(1u<<13) && p.speeds[13]==USB_SPEED_SUPER_PLUS);
    assert(p.rates[13]==10000000000ull && p.psi_count==1 && p.psi[0]==psi);
}

/* Different RsvdP fields do not invalidate a well-formed Rx/Tx pair. */
static void noisy_asymmetric(void)
{
    struct xhci_protocol p;
    uint32_t psi[]={ (10u<<16)|(1u<<14)|(0x15u<<9)|(1u<<8)|(2u<<6)|(3u<<4)|9u,
                     (20u<<16)|(1u<<14)|(0x0au<<9)|(1u<<8)|(3u<<6)|(3u<<4)|9u };
    assert(!parse_usb(3,0x20,psi,2,&p));
    assert(!(p.valid_speeds&(1u<<9)) && p.psi_count==2);
    assert(p.psi[0]==psi[0] && p.psi[1]==psi[1]);
}

/* A structurally valid Rx/Tx pair stays unavailable; malformed pairs fail. */
static void asymmetric_pairs(void)
{
    struct xhci_protocol p;
    uint32_t psi[]={ (10u<<16)|(1u<<14)|(1u<<8)|(2u<<6)|(3u<<4)|9u,
                     (20u<<16)|(1u<<14)|(1u<<8)|(3u<<6)|(3u<<4)|9u };
    assert(!parse_usb(3,0x20,psi,2,&p) && !(p.valid_speeds&(1u<<9)));
    assert(p.psi_count==2 && p.psi[0]==psi[0] && p.psi[1]==psi[1]);
    psi[1]^=1u;
    assert(parse_usb(3,0x20,psi,2,&p)==-EINVAL);
    psi[1]^=1u;psi[1]^=1u<<8;
    assert(parse_usb(3,0x20,psi,2,&p)==-EINVAL);
    psi[1]^=1u<<8;psi[1]^=1u<<14;
    assert(parse_usb(3,0x20,psi,2,&p)==-EINVAL);
    assert(parse_usb(3,0x20,psi,1,&p)==-EINVAL);
}

int main(int argc, char **argv)
{
    if (argc==2) {
        if (!strcmp(argv[1],"implied")) implied_speeds();
        else if (!strcmp(argv[1],"explicit")) explicit_speeds();
        else if (!strcmp(argv[1],"reject")) rejected_speeds();
        else if (!strcmp(argv[1],"pairs")) asymmetric_pairs();
        else if (!strcmp(argv[1],"noisy-symmetric")) noisy_symmetric();
        else if (!strcmp(argv[1],"noisy-pair")) noisy_asymmetric();
        else return 2;
        return 0;
    }
    implied_speeds();explicit_speeds();rejected_speeds();asymmetric_pairs();
    noisy_symmetric();noisy_asymmetric();
    uint32_t regs[64]={0};struct xhci_protocol ports[8];
    regs[16]=(2u<<24)|(6u<<8)|2u;regs[17]=0x20425355u;regs[18]=(2u<<8)|1u;
    regs[22]=(3u<<24)|2u;regs[23]=0x20425355u;regs[24]=(2u<<8)|4u;
    assert(!xhci_protocol_parse(regs,sizeof(regs),64,8,ports));
    assert(ports[0].major==2 && ports[1].major==2 && !ports[2].major);
    assert(ports[3].major==3 && ports[4].major==3 && !ports[5].major);
    assert(ports[0].valid_speeds==0x0e && ports[0].speeds[1]==0 &&
           ports[0].speeds[2]==1 && ports[0].speeds[3]==2);
    assert(ports[3].valid_speeds==0x10 && ports[3].speeds[4]==3 &&
           ports[3].rates[4]==5000000000ull);
    regs[24]=(2u<<8)|2u; /* ranges must never overlap */
    assert(xhci_protocol_parse(regs,sizeof(regs),64,8,ports)==-EINVAL);
    regs[24]=(2u<<8)|4u;regs[16]=(2u<<24)|(2u<<8)|2u; /* next cap inside descriptor */
    assert(xhci_protocol_parse(regs,sizeof(regs),64,8,ports)==-EINVAL);
    regs[16]=(2u<<24)|(6u<<8)|2u;
    regs[24]=(2u<<8)|4u|(2u<<28); /* explicit PSI, no implicit speed IDs */
    regs[26]=(5u<<16)|(1u<<8)|(3u<<4)|9u;
    regs[27]=(10u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|10u;
    assert(!xhci_protocol_parse(regs,sizeof(regs),64,8,ports));
    assert(ports[3].valid_speeds==(1u<<9) && ports[3].speeds[9]==3);
    regs[27]=regs[26]; /* repeated symmetric PSIV */
    assert(xhci_protocol_parse(regs,sizeof(regs),64,8,ports)==-EINVAL);
    regs[27]=(5u<<16)|(1u<<8)|(2u<<6)|(3u<<4)|10u; /* unpaired asymmetric Rx */
    assert(xhci_protocol_parse(regs,sizeof(regs),64,8,ports)==-EINVAL);
    assert(xhci_protocol_parse(regs,92,64,8,ports)==-EINVAL);
    puts("USB xHCI supported protocol tests passed");
}
