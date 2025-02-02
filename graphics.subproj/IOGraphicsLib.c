/*
 * Copyright (c) 1998-2000, 2012 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <sys/cdefs.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_switch.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach_time.h>
#include <syslog.h>
#include <asl.h>

#include <CoreFoundation/CoreFoundation.h>

#include <IOKit/IOKitLib.h>
#include <libkern/OSByteOrder.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOCFURLAccess.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <IOKit/graphics/IOGraphicsLibPrivate.h>
#include <IOKit/graphics/IOGraphicsTypesPrivate.h>
#include <IOKit/graphics/IOAccelSurfaceControl.h>
#include <IOKit/graphics/IOAccelSurfaceConnect.h>
#include <IOKit/IOHibernatePrivate.h>
#include "IOGraphicsLibInternal.h"

#ifndef kIOFBDependentIDKey
#define kIOFBDependentIDKey     "IOFBDependentID"
#endif
#ifndef kIOFBDependentIndexKey
#define kIOFBDependentIndexKey  "IOFBDependentIndex"
#endif

#define FILTER_MAXDEPTH                 32
#ifndef kIOFBModePIKey
#define kIOFBModePIKey                  "PI"
#endif


#define arrayCnt(var) (sizeof(var) / sizeof(var[0]))

enum {
    kAquaMinWidth  = 800,
    kAquaMinHeight = 600,
    kInstallMinWidth  = 1024,
    kInstallMinHeight = 768
};

enum {
    kMirrorOnlyFlags = (kDisplayModeValidForAirPlayFlag | kDisplayModeValidForMirroringFlag),
    kAddSafeFlags    = (kDisplayModeValidFlag | kDisplayModeValidateAgainstDisplay)
};

enum { kIOFBSWOfflineDisplayModeID = (IODisplayModeID) 0xffffff00 };

#define kAppleSetupDonePath     "/var/db/.AppleSetupDone"
#define kIOFirstBootFlagPath    "/var/db/.com.apple.iokit.graphics"

#define kIOGraphicsLogfilePath      "/var/log/.com.apple.iokit.graphics.log"
#define kIOGraphicsDesktopImagePath "/private/var/tmp/desktop.tga"
#define kIOGraphicsLockImagePath    "/private/var/tmp/screenlock.tga"

#define DEBUG_NO_DRIVER_MODES   0

struct DMTimingOverrideRec {
 UInt32 timingOverrideVersion;
 UInt32 timingOverrideAttributes;   // flags
 UInt32 timingOverrideSetFlags;     // VDTimingInfoRec.csTimingFlags |= timingOverrideSetFlags
 UInt32 timingOverrideClearFlags;   // VDTimingInfoRec.csTimingFlags &= (~timingOverrideClearFlags)
 UInt32 timingOverrideReserved[16]; // reserved
};
typedef struct DMTimingOverrideRec      DMTimingOverrideRec;

struct DMDisplayTimingInfoRec {
 UInt32 timingInfoVersion;
 UInt32 timingInfoAttributes;       // flags
 SInt32 timingInfoRelativeQuality;  // quality of the timing
 SInt32 timingInfoRelativeDefault;  // relative default of the timing
 UInt32 timingInfoReserved[16];     // reserved
};
typedef struct DMDisplayTimingInfoRec   DMDisplayTimingInfoRec;

#define desireDPI       (75.0)
#define mmPerInch       (25.4)

static kern_return_t 
IOFramebufferServerOpen( mach_port_t connect );
kern_return_t
IOFramebufferServerFinishOpen( io_connect_t connect );
static kern_return_t
IOFramebufferFinishOpen( IOFBConnectRef connectRef );

static kern_return_t
IOFBLookDefaultDisplayMode( IOFBConnectRef connectRef );

static void
IOFBCreateOverrides( IOFBConnectRef connectRef );

static kern_return_t
IOFBCreateDisplayModeInformation(
        IOFBConnectRef                  connectRef,
        IODisplayModeID                 displayMode,
        IOFBDisplayModeDescription *    allInfo );

static kern_return_t
IOFBAdjustDisplayModeInformation(
        IOFBConnectRef                  connectRef,
        IODisplayModeID                 displayMode,
        IOFBDisplayModeDescription *    allInfo );

static IOIndex
IOFBIndexForPixelBits( IOFBConnectRef connectRef, IODisplayModeID mode,
                                      IOIndex maxIndex, UInt32 bpp );

static Boolean
IOFBLookScaleBaseMode( IOFBConnectRef connectRef,
                        IOFBDisplayModeDescription * scaleBase,
                        IOFBDisplayModeDescription * scaleDesc );
static kern_return_t
IOFBInstallScaledModes( IOFBConnectRef connectRef,
                        IOFBDisplayModeDescription * scaleBase,
                        Boolean onlyMirrorModes );
static kern_return_t
IOFBInstallScaledMode( IOFBConnectRef connectRef,
                       IOFBDisplayModeDescription * desc,
                       IOOptionBits installFlags );

static kern_return_t
_IOFBGetAttributeForFramebuffer( io_connect_t connect, io_connect_t otherConnect,
                                IOSelect attribute, UInt32 * value );

static kern_return_t
__IOFBGetPixelInformation(
        IOFBConnectRef          connectRef,
        IODisplayModeID         displayMode,
        IOIndex                 depth,
        IOPixelAperture         aperture,
        IOPixelInformation *    pixelInfo );

static kern_return_t
_IOFBGetPixelInformation(
        IOFBConnectRef          connectRef,
        IODisplayModeID         displayMode,
        IOIndex                 depth,
        IOPixelAperture         aperture,
        IOPixelInformation *    pixelInfo );


static kern_return_t
__IOFBGetCurrentDisplayModeAndDepth( IOFBConnectRef connectRef,
        IODisplayModeID * displayMode,
        IOIndex         * depth );

static kern_return_t
_IOFBGetCurrentDisplayModeAndDepth( IOFBConnectRef connectRef,
        IODisplayModeID * displayMode,
        IOIndex         * depth );

static kern_return_t
IOFBResetTransform( IOFBConnectRef connectRef );

static bool
IOFBWritePrefs( IOFBConnectRef connectRef );

__private_extern__ CFMutableDictionaryRef   gIOGraphicsProperties = 0;

static struct IOFBConnect *                 gAllConnects = 0;
static CFMutableDictionaryRef               gConnectRefDict = 0;
static bool                                 gIOGraphicsSentPrefs = false;
static io_service_t                         gIOGraphicsPrefsService;
static bool                                 gIOGraphicsInstallBoot = false;
static const char *						    gIOGraphicsImageFiles[2] = { 
															kIOGraphicsDesktopImagePath, 
															kIOGraphicsLockImagePath };

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct IOFBMakeNumKeysContext
{
    CFMutableDictionaryRef dict;
    Boolean                andValues;
};

static void
IOFBMakeNumKeys( const void * key, const void * value, void * context )
{
    CFStringRef                 str = key;
    CFMutableDictionaryRef      newDict   = ((struct IOFBMakeNumKeysContext *)context)->dict;
    Boolean                     andValues = ((struct IOFBMakeNumKeysContext *)context)->andValues;
    const char *                cStr;
    char *                      buffer = NULL;

    cStr = CFStringGetCStringPtr( str, kCFStringEncodingMacRoman);
    if( !cStr) {
        CFIndex bufferSize = CFStringGetMaximumSizeForEncoding( CFStringGetLength(str),
               kCFStringEncodingMacRoman) + sizeof('\0');
        buffer = malloc( bufferSize);
        if( buffer && CFStringGetCString( str, buffer, bufferSize, kCFStringEncodingMacRoman))
            cStr = buffer;
    }
    if( cStr)
        key = (const void *) (unsigned long) strtol( cStr, 0, 0 );
    else
        key = 0;
    if( buffer)
        free( buffer);

    if (!key)
        return;

    if (andValues)
    {
        SInt32 scalarValue;
        CFNumberGetValue(value, kCFNumberSInt32Type, &scalarValue);
        value = (const void *)(uintptr_t) scalarValue;
    }
        
    CFDictionarySetValue(newDict, key, value);
}

static CFMutableDictionaryRef
IOFBMakeIntegerKeys( CFDictionaryRef dict, Boolean andValues )
{
    struct IOFBMakeNumKeysContext context;

    context.dict      = 0;
    context.andValues = andValues;

    if( dict && (context.dict = CFDictionaryCreateMutable( 
                                kCFAllocatorDefault, (CFIndex) 0,
                                (CFDictionaryKeyCallBacks *) 0,
                                andValues ? (CFDictionaryValueCallBacks *) 0
                                : &kCFTypeDictionaryValueCallBacks )))
        CFDictionaryApplyFunction( dict, &IOFBMakeNumKeys, &context );

    return (context.dict);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void IOLoadPEFsFromURL( CFURLRef ndrvDirURL, io_service_t service );

static void ScanNDRVs( io_service_t service )
{
    io_registry_entry_t root;
    CFURLRef            url;
    Boolean             scan = false;
    CFDataRef           data;
    UInt32 *            gen;

    root = IORegistryGetRootEntry(kIOMasterPortDefault);
    if (root)
    {
        data = (CFDataRef) IORegistryEntryCreateCFProperty(root,
                                        CFSTR(kIONDRVFramebufferGenerationKey),
                                        kCFAllocatorDefault, kNilOptions);
        if (data)
        {
            gen  = (UInt32 *) CFDataGetBytePtr(data);
            scan = (gen[0] != gen[1]);
            CFRelease(data);
        }
        IOObjectRelease(root);
    }

    if (scan)
    {
        url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                    CFSTR("/System/Library/Extensions/AppleNDRV"), kCFURLPOSIXPathStyle, true);
        if (url)
        {
            IOLoadPEFsFromURL(url, service);
            CFRelease(url);
        }
        gIOGraphicsSentPrefs = false;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

kern_return_t
IOFramebufferServerStart( void )
{
    CFMutableDictionaryRef newDict;
    CFMutableDictionaryRef prefs = 0;

    ScanNDRVs( MACH_PORT_NULL );

    if (!gIOGraphicsProperties)
    {
        gIOGraphicsProperties = readPlist("/System/Library/Frameworks/IOKit.framework/"
                                            "Resources/IOGraphicsProperties.plist", 0);
        if (gIOGraphicsProperties)
        {
            if ((newDict = IOFBMakeIntegerKeys(CFDictionaryGetValue(gIOGraphicsProperties,
                                                        CFSTR("std-modes")), false)))
            {
                CFDictionarySetValue(gIOGraphicsProperties, CFSTR("std-modes"), newDict);
                CFRelease( newDict );
            }
            if ((newDict = IOFBMakeIntegerKeys(CFDictionaryGetValue(gIOGraphicsProperties, 
                                                        CFSTR("timing-ids")), true)))
            {
                CFDictionarySetValue(gIOGraphicsProperties, CFSTR("timing-ids"), newDict);
                CFRelease( newDict );
            }
            if ((newDict = IOFBMakeIntegerKeys(CFDictionaryGetValue(gIOGraphicsProperties, 
                                                        CFSTR("irb-timing-ids")), true)))
            {
                CFDictionarySetValue(gIOGraphicsProperties, CFSTR("irb-timing-ids"), newDict);
                CFRelease( newDict );
            }
        }
        else
            gIOGraphicsProperties = CFDictionaryCreateMutable( kCFAllocatorDefault, 0,
                                                                &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);

        gIOGraphicsPrefsService = IORegistryEntryFromPath(kIOMasterPortDefault, 
                                        kIOServicePlane ":/IOResources/IODisplayWrangler");

        int sbmib[] = { CTL_KERN, KERN_SAFEBOOT };
        uint32_t value = 0;
        size_t vsize = sizeof(value);
        Boolean safeBoot = (-1 != sysctl(sbmib, 2, &value, &vsize, NULL, 0)) && (value != 0);

        if (!safeBoot)
            prefs = readPlist(kIOFirstBootFlagPath, 0);
        if (!prefs)
            prefs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
        if (prefs)
        {
            CFDictionarySetValue(gIOGraphicsProperties, CFSTR("prefs"), prefs);
            CFRelease(prefs);

            if (!gIOGraphicsSentPrefs)
            {
                gIOGraphicsSentPrefs = true;
                if (gIOGraphicsPrefsService)
                    IORegistryEntrySetCFProperty(gIOGraphicsPrefsService, CFSTR(kIOGraphicsPrefsKey), prefs); 
            }
        }
        struct stat stat_buf;
        gIOGraphicsInstallBoot = (0 != stat(kAppleSetupDonePath, &stat_buf));
    }

    return( KERN_SUCCESS );
}

kern_return_t
IOFramebufferOpen(
        io_service_t    service,
        task_port_t     owningTask,
        unsigned int    type,
        io_connect_t  * connect )
{
    kern_return_t       kr;

    kr = IOServiceOpen( service, owningTask, type, connect );

    if ((KERN_SUCCESS == kr) && (type == kIOFBServerConnectType))
        kr = IOFramebufferServerOpen( *connect );

    return( kr );
}


kern_return_t
IOFBCreateSharedCursor( io_connect_t connect,
        unsigned int version,
        unsigned int maxWidth, unsigned int maxHeight )
{

	IOFramebufferServerOpen( connect );
    IOFramebufferServerFinishOpen( connect );

    uint64_t inData[] = { version, maxWidth, maxHeight };
    return IOConnectCallMethod(connect, 0,                      // Index
                inData, arrayCnt(inData), NULL, 0,      // Input
                NULL,   NULL,             NULL, NULL);  // Output
}

extern kern_return_t
IOFBGetFramebufferInformationForAperture( io_connect_t connect,
            IOPixelAperture               aperture,
            IOFramebufferInformation    * info )
{
    IOFBConnectRef      connectRef = IOFBConnectToRef( connect );
    IOPixelInformation  pixelInfo;
    IODisplayModeID     mode;
    IOIndex             depth;
    kern_return_t       err;

    if (!connectRef)
        return (kIOReturnBadArgument);

    err = _IOFBGetCurrentDisplayModeAndDepth(connectRef, &mode, &depth);
    if( err)
        return( err);
    err = _IOFBGetPixelInformation(connectRef, mode, depth, aperture, &pixelInfo);
    if( err)
        return( err);

    err = IOFBGetFramebufferOffsetForAperture( connect, aperture,
                                                &info->baseAddress);
    info->activeWidth   = pixelInfo.activeWidth;
    info->activeHeight  = pixelInfo.activeHeight;
    info->bytesPerRow   = pixelInfo.bytesPerRow;
    info->bytesPerPlane = pixelInfo.bytesPerPlane;
    info->bitsPerPixel  = pixelInfo.bitsPerPixel;
    info->pixelType     = pixelInfo.pixelType;
    info->flags         = pixelInfo.flags;

    return( err);
}

extern kern_return_t
IOFBGetFramebufferOffsetForAperture( mach_port_t connect,
            IOPixelAperture               aperture,
            IOByteCount                 * offset )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if (!connectRef)
        return (kIOReturnBadArgument);
    
    if (kIOFBConnectStateUnusable & connectRef->state)
    {
        *offset = 0;
        return (kIOReturnSuccess);
    }
    uint64_t inData  = aperture;
    uint64_t outData = 0;
    uint32_t outSize = 1;
    kern_return_t kr = IOConnectCallMethod(connect, 8,  // Index
                &inData,  1,        NULL, 0,            // Input
                &outData, &outSize, NULL, NULL);        // Output
    *offset = (IOByteCount) outData;
    return kr;
}

extern kern_return_t
IOFBSetBounds( io_connect_t connect,
              IOGBounds   * rect )
{
    IOGBounds   rects[2];
    rects[0] = rects[1] = *rect;
    return IOConnectCallMethod(connect, 9,              // Index
                               NULL,    0, rects, sizeof(rects),       // Input
                               NULL, NULL, NULL, NULL);                // Output
}

extern kern_return_t
IOFBSetVirtualBounds(io_connect_t connect,
                     IOGBounds   * screenBounds,
                     IOGBounds   * desktopBounds )
{
    IOGBounds   rects[2];
    rects[0] = *screenBounds;
    rects[1] = *desktopBounds;
    return IOConnectCallMethod(connect, 9,              // Index
                               NULL,    0, rects, sizeof(rects),       // Input
                               NULL, NULL, NULL, NULL);                // Output
}

static kern_return_t
__IOFBGetCurrentDisplayModeAndDepth( IOFBConnectRef connectRef,
        IODisplayModeID * displayMode,
        IOIndex         * depth )
{
    kern_return_t err;
    uint64_t outData[2];
    uint32_t outSize = arrayCnt(outData);

    err = IOConnectCallMethod(connectRef->connect, 2,   // Index
                NULL,           0, NULL,    0,          // Input
                outData, &outSize, NULL, NULL);         // Output
    *displayMode = (IODisplayModeID) outData[0];
    *depth       = (IOIndex)         outData[1];

    DEBG(connectRef, "IOFBGetCurrentDisplayModeAndDepth(%x), %x, %d\n", err, (int) *displayMode, (int) *depth);

    return( err );
}

static kern_return_t
_IOFBGetCurrentDisplayModeAndDepth( IOFBConnectRef connectRef,
        IODisplayModeID * displayMode,
        IOIndex         * depth )
{
    if (kIOFBConnectStateUnusable & connectRef->state)
    {
        *displayMode = kIOFBSWOfflineDisplayModeID;
        *depth       = 0;
        return (kIOReturnSuccess);
    }
    else return (__IOFBGetCurrentDisplayModeAndDepth(connectRef, displayMode, depth));
}

kern_return_t
IOFBGetCurrentDisplayModeAndDepth( io_connect_t connect,
        IODisplayModeID * displayMode,
        IOIndex         * depth )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );
    if (!connectRef)
        return (kIOReturnBadArgument);

    return (_IOFBGetCurrentDisplayModeAndDepth(connectRef, displayMode, depth));
}

extern kern_return_t
IOFBGetPixelFormat( io_connect_t connect,
        IODisplayModeID mode,
        IOIndex         depth,
        IOPixelAperture aperture,
        IOPixelEncoding * pixelFormat )
{
    IOPixelInformation  pixelInfo;
    kern_return_t       err;

    err = IOFBGetPixelInformation( connect, mode, depth, aperture, &pixelInfo );
    if( err)
        return( err);

    strncpy( *pixelFormat, pixelInfo.pixelFormat, kIOMaxPixelBits );

    return( err);
}

extern kern_return_t
IOFBSetCLUT( io_connect_t connect,
        UInt32          startIndex,
        UInt32          numEntries,
        IOOptionBits    options,
        IOColorEntry *  colors )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );
    if (!connectRef)
        return (kIOReturnBadArgument);

    if (kIOFBConnectStateUnusable & connectRef->state)
    {
        return (kIOReturnSuccess);
    }

    uint64_t inData[] = { startIndex, options };
    size_t   inSize = numEntries * sizeof( IOColorEntry);
    return IOConnectCallMethod(connect, 16,                             // Index
                inData, arrayCnt(inData), colors, inSize,       // Input
                NULL, NULL, NULL, NULL);                        // Output
}

extern kern_return_t
IOFBSetGamma( io_connect_t connect,
        UInt32          channelCount,
        UInt32          dataCount,
        UInt32          dataWidth,
        void *          data )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );
    if (!connectRef)
        return (kIOReturnBadArgument);

    if (kIOFBConnectStateUnusable & connectRef->state)
    {
        return (kIOReturnSuccess);
    }

    uint64_t inData[] = { channelCount, dataCount, dataWidth };
    size_t   inSize = ((dataWidth + 7) / 8) * dataCount * channelCount;

    return IOConnectCallMethod(connect, 11,             // Index
                inData, arrayCnt(inData), data, inSize, // Input
                NULL, NULL, NULL, NULL);                // Output
}

static IOOptionBits
IOFBGetState( IOFBConnectRef connectRef )
{
    IOOptionBits        state = 0;
    io_service_t        display;

    if( (display = IODisplayForFramebuffer( connectRef->framebuffer, kNilOptions))) {
        state |= (kIOFBConnectStateHWOnline | kIOFBConnectStateOnline);
        IOObjectRelease( display );
    }
    if (connectRef)
        DEBG(connectRef, "IOFBGetState(%p) = %08x\n",
                connectRef, (int) state);

    return( state );
}

extern kern_return_t
IOFBSet444To555Table( io_connect_t connect,
        const unsigned char *   table )
{
    uint64_t inData = 0;
    size_t   inSize = 16 * sizeof(table[0]);
    return IOConnectCallMethod(connect, 15,             // Index
                &inData, 1, table, inSize,              // Input
                NULL, NULL, NULL,  NULL);               // Output
}

extern kern_return_t
IOFBSet555To444Table( io_connect_t connect,
        const unsigned char *   table )
{
    uint64_t inData = 1;
    size_t   inSize = 32 * sizeof(table[0]);
    return IOConnectCallMethod(connect, 15,             // Index
                &inData, 1, table, inSize,              // Input
                NULL, NULL, NULL,  NULL);               // Output
}

extern kern_return_t
IOFBSet256To888Table( io_connect_t connect,
        const unsigned int *    table )
{
    uint64_t inData = 2;
    size_t   inSize = 256 * sizeof(table[0]);
    return IOConnectCallMethod(connect, 15,             // Index
                &inData, 1, table, inSize,              // Input
                NULL, NULL, NULL,  NULL);               // Output
}

extern kern_return_t
IOFBSet888To256Table( io_connect_t connect,
        const unsigned char *   table )
{
    uint64_t inData = 3;
    size_t   inSize = 5 * 256 * sizeof(table[0]);
    return IOConnectCallMethod(connect, 15,             // Index
                &inData, 1, table, inSize,              // Input
                NULL, NULL, NULL,  NULL);               // Output
}

static kern_return_t
_IOFBGetDisplayModeCount( IOFBConnectRef connectRef,
        UInt32 * count )
{
    if (kIOFBConnectStateUnusable & connectRef->state)
    {
        *count = 1;
        return (kIOReturnSuccess);
    }

    uint64_t outData = 0;
    uint32_t outCnt  = 1;

    kern_return_t kr =  IOConnectCallMethod(connectRef->connect, 6, // Index
                NULL,           0, NULL,     0,         // Input
                &outData, &outCnt, NULL,  NULL);        // Output
    *count = (UInt32) outData;
    return kr;
}

static kern_return_t
_IOFBGetDisplayModes( IOFBConnectRef connectRef,
        UInt32                  count,
        IODisplayModeID *       allDisplayModes )
{
    if (kIOFBConnectStateUnusable & connectRef->state)
    {
         if (1 != count)
            return (kIOReturnBadArgument);
        *allDisplayModes = kIOFBSWOfflineDisplayModeID;
        return (kIOReturnSuccess);
    }

    size_t len = count * sizeof( IODisplayModeID);
    return IOConnectCallMethod(connectRef->connect, 7,              // Index
                NULL,    0, NULL,            0,         // Input
                NULL, NULL, allDisplayModes, &len);     // Output
}


kern_return_t
IOFBGetDisplayModeCount( io_connect_t connect,
        UInt32 * count )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    *count = CFArrayGetCount( connectRef->modesArray );

    return( kIOReturnSuccess );
}


kern_return_t
IOFBGetDisplayModes( io_connect_t connect,
        UInt32                  count,
        IODisplayModeID *       allDisplayModes )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );
    UInt32         i, modeCount;

    modeCount = CFArrayGetCount( connectRef->modesArray );
    if( count < modeCount)
        modeCount = count;

    for( i = 0; i < modeCount; i++ ) {
        CFDictionaryRef dict;
        CFNumberRef     num;

        dict = (CFMutableDictionaryRef) CFArrayGetValueAtIndex( connectRef->modesArray, i );
        num = CFDictionaryGetValue( dict, CFSTR(kIOFBModeIDKey) );
        if( num)
            CFNumberGetValue( num, kCFNumberSInt32Type, (SInt32 *) &allDisplayModes[i] );
        else
            allDisplayModes[i] = 0;
    }

    return( kIOReturnSuccess );
}

static kern_return_t
_IOFBGetAttributeForFramebuffer( io_connect_t connect, io_connect_t otherConnect,
                                IOSelect attribute, UInt32 * value )
{
    IOReturn err;

    if( otherConnect) {
        err = IOConnectAddClient( connect, otherConnect );
        if( err)
            return( err );
    }

    uint64_t inData  = attribute;
    uint64_t outData = 0;
    uint32_t outCnt  = 1;
    err = IOConnectCallMethod(connect, 18,      // Index
                &inData,  1,       NULL,    0,  // Input
                &outData, &outCnt, NULL, NULL); // Output
    *value = (UInt32) outData;

    return( err );
}

kern_return_t
IOFBGetAttributeForFramebuffer( io_connect_t connect, io_connect_t otherConnect,
                                IOSelect attribute, UInt32 * value )
{
    IOReturn err;
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if (!connectRef)
        return (kIOReturnBadArgument);
    
    if (kIOFBConnectStateUnusable & connectRef->state)
    {
        *value = 0;
        if (kIOMirrorDefaultAttribute == attribute)
            err = kIOReturnSuccess;
        else
            err = kIOReturnOffline;
    }
    else
    {
        switch (attribute)
        {
            case kIOFBMatchedConnectChangeAttribute:
                *value = (connectRef->matchMode != kIODisplayModeIDInvalid);
                err = kIOReturnSuccess;
                break;

            default:    
                err = _IOFBGetAttributeForFramebuffer( connect, otherConnect, attribute, value );
                break;
        }
    }

    if ((kIOReturnSuccess == err)
     && (kIOMirrorDefaultAttribute == attribute)
     && connectRef 
     && connectRef->displayMirror)
    {
        *value |= kIOMirrorDefault;
    }
    if (kIOMirrorDefaultAttribute == attribute)
        DEBG(connectRef, "kIOMirrorDefaultAttribute = %08x (%p, %p)\n",
                (int) *value, connectRef, connectRef ? connectRef->overrides : 0);

    return( err );
}

static void 
IOFBDictRemoveModePI(const void *key __unused, const void *value, void *context __unused)
{
    CFDictionaryRemoveValue((CFMutableDictionaryRef) value, CFSTR(kIOFBModePIKey));
}

kern_return_t
IOFBSetAttributeForFramebuffer( io_connect_t connect, io_connect_t otherConnect,
                                IOSelect attribute, UInt32 value )
{
    IOReturn err;
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if (!connectRef)
        return (kIOReturnBadArgument);

    if (kIOFBConnectStateUnusable & connectRef->state)
        return (kIOReturnSuccess);

    if( otherConnect) {
        err = IOConnectAddClient( connect, otherConnect );
        if( err)
            return( err );
    }

    if (kIOMirrorAttribute == attribute)
    {
        DEBG(connectRef, "set mirror %d\n", (int) value);
    }

    uint64_t inData[] = { attribute, value };
    err = IOConnectCallMethod(connect, 19,              // Index
                inData, arrayCnt(inData), NULL,    0,   // Input
                NULL,   NULL,             NULL, NULL);  // Output

    if (kIOMirrorAttribute == attribute)
    {
        DEBG(connectRef, "did set mirror(%x)\n", err);
        IOFBResetTransform(connectRef);

        CFDictionaryApplyFunction(connectRef->modes, &IOFBDictRemoveModePI, NULL);
		if (otherConnect)
		{
			IOFBConnectRef otherConnectRef = IOFBConnectToRef( otherConnect );
			if (otherConnectRef) CFDictionaryApplyFunction(otherConnectRef->modes, &IOFBDictRemoveModePI, NULL);
		}
    }

    return( err );
}

__private_extern__ float
ratioOver( float a, float b )
{
    if( a > b)
        return( a / b );
    else
        return( b / a );
}

__private_extern__ Boolean
ValidateTimingInformation( IOFBConnectRef connectRef __unused, const IOTimingInformation * timingInfo )
{
    if (!timingInfo->detailedInfo.v2.horizontalActive
     || !timingInfo->detailedInfo.v2.verticalActive
     || ((timingInfo->detailedInfo.v2.horizontalSyncOffset + timingInfo->detailedInfo.v2.horizontalSyncPulseWidth) 
            > timingInfo->detailedInfo.v2.horizontalBlanking)
     || ((timingInfo->detailedInfo.v2.verticalSyncOffset + timingInfo->detailedInfo.v2.verticalSyncPulseWidth) 
            > timingInfo->detailedInfo.v2.verticalBlanking)
     || !timingInfo->detailedInfo.v2.verticalActive)
    {
#if RLOG
        DEBG(connectRef, "!ValidateTimingInformation\n");
        IOFBLogTiming(connectRef, timingInfo);
#endif
        return (false);
    }

    return (true);
}

// timing is bad enough it should be ignored regardless of source
__private_extern__ Boolean
InvalidTiming( IOFBConnectRef connectRef __unused, const IOTimingInformation * timingInfo )
{

    if ((kIODetailedTimingValid & timingInfo->flags)
     && (!timingInfo->detailedInfo.v2.horizontalSyncPulseWidth
        || !timingInfo->detailedInfo.v2.verticalSyncPulseWidth))
    {
#if RLOG
        DEBG(connectRef, "InvalidTiming\n");
        IOFBLogTiming(connectRef, timingInfo);
#endif
        return (true);
    }

    return (false);
}

__private_extern__ float
RefreshRateFromDetailedTiming( IODetailedTimingInformationV2 * detailed )
{
    float rate;

    rate =   ((float) detailed->pixelClock)
           / ((float)(detailed->horizontalActive + detailed->horizontalBlanking))
           / ((float)(detailed->verticalActive   + detailed->verticalBlanking));

    if (kIOInterlacedCEATiming & detailed->signalConfig)
        rate *= 2.0;

    return (rate);
}

static void
MakeDetailedRefresh( IOFBConnectRef connectRef, IOFBDisplayModeDescription * modeInfo )
{
    if (!(kIODetailedTimingValid & modeInfo->timingInfo.flags))
        return;
//    if (kDisplayModeTelevisionFlag & modeInfo->info.flags)
//      return;

    if (false
      || (kIOTimingIDAppleNTSC_ST     == modeInfo->timingInfo.appleTimingID)
      || (kIOTimingIDAppleNTSC_FF     == modeInfo->timingInfo.appleTimingID)
      || (kIOTimingIDAppleNTSC_STconv == modeInfo->timingInfo.appleTimingID)
      || (kIOTimingIDAppleNTSC_FFconv == modeInfo->timingInfo.appleTimingID)
      || (kIOTimingIDApplePAL_ST      == modeInfo->timingInfo.appleTimingID)
      || (kIOTimingIDApplePAL_FF      == modeInfo->timingInfo.appleTimingID)
      || (kIOTimingIDApplePAL_STconv  == modeInfo->timingInfo.appleTimingID)
      || (kIOTimingIDApplePAL_FFconv  == modeInfo->timingInfo.appleTimingID))
        return;

    if (!ValidateTimingInformation(connectRef, &modeInfo->timingInfo))
        return;

    modeInfo->info.refreshRate = 65536ULL * modeInfo->timingInfo.detailedInfo.v2.pixelClock
                                        / ((UInt64)(modeInfo->timingInfo.detailedInfo.v2.horizontalActive
                                            + modeInfo->timingInfo.detailedInfo.v2.horizontalBlanking))
                                        / ((UInt64)(modeInfo->timingInfo.detailedInfo.v2.verticalActive
                                            + modeInfo->timingInfo.detailedInfo.v2.verticalBlanking));

    if (kDisplayModeInterlacedFlag & modeInfo->info.flags)
        modeInfo->info.refreshRate *= 2;
}

enum
{
    kDetailedTimingsNotEqual = false,
    kDetailedTimingsEqual = true,
    kDetailedTimingsIdentical = 2
};

static Boolean
DetailedTimingsEqual( IODetailedTimingInformationV2 * newTimingInfo,
                      IODetailedTimingInformationV2 * existingTimingInfo,
                      IOOptionBits modeGenFlags )
{
    Boolean existingScaled, newScaled;
    UInt32 vMask;
    UInt32 eH, eV, eF, nH, nV, nF, swap;

    eH = existingTimingInfo->horizontalScaled;
    eV = existingTimingInfo->verticalScaled;
    eF = existingTimingInfo->scalerFlags;
    nH = newTimingInfo->horizontalScaled;
    nV = newTimingInfo->verticalScaled;
    nF = newTimingInfo->scalerFlags;

    existingScaled = (eH && eV);
    newScaled      = (nH && nV);

    if (kIOInterlacedCEATiming & (newTimingInfo->signalConfig ^ existingTimingInfo->signalConfig))
        return (false);

    if (kIOFBGTFMode & modeGenFlags)
    {
        UInt32 new, existing;

        existing = existingScaled ? existingTimingInfo->horizontalScaled
                                  : existingTimingInfo->horizontalActive;
        new      = newScaled      ? newTimingInfo->horizontalScaled
                                  : newTimingInfo->horizontalActive;
        if( new < (existing - 20))
            return (kDetailedTimingsNotEqual);
        if( new > (existing + 20))
            return (kDetailedTimingsNotEqual);

        existing = existingScaled ? existingTimingInfo->verticalScaled
                                  : existingTimingInfo->verticalActive;
        new      = newScaled      ? newTimingInfo->verticalScaled
                                  : newTimingInfo->verticalActive;

        if( new < (existing - 20))
            return (kDetailedTimingsNotEqual);
        if( new > (existing + 20))
            return (kDetailedTimingsNotEqual);

        if (fabsf(RefreshRateFromDetailedTiming(newTimingInfo) 
                - RefreshRateFromDetailedTiming(existingTimingInfo)) < 1.0)
            return (kDetailedTimingsEqual);
            
        return (kDetailedTimingsNotEqual);
    }

    if (existingScaled || newScaled)
    {
        if (kIOFBTimingMatch & modeGenFlags)
        {
            if (kIOScaleSwapAxes & eF)
            {
                swap = eH;
                eH = eV;
                eV = swap;
            }
            eF &= ~kIOScaleRotateFlags;
            if (eH == existingTimingInfo->horizontalActive)
                eH = 0;
            if (eV == existingTimingInfo->verticalActive)
                eV = 0;
            if (kIOScaleSwapAxes & nF)
            {
                swap = nH;
                nH = nV;
                nV = swap;
            }
            nF &= ~kIOScaleRotateFlags;
            if (nH == newTimingInfo->horizontalActive)
                nH = 0;
            if (nV == newTimingInfo->verticalActive)
                nV = 0;
        }
        if (eF != nF)
            return (kDetailedTimingsNotEqual);
        if (eH != nH)
            return (kDetailedTimingsNotEqual);
        if (eV != nV)
            return (kDetailedTimingsNotEqual);
    }

    if (kIOInterlacedCEATiming 
        & newTimingInfo->signalConfig & existingTimingInfo->signalConfig)
        vMask = ~1;
    else
        vMask = ~0;

    if (newTimingInfo->horizontalActive != existingTimingInfo->horizontalActive)
        return (kDetailedTimingsNotEqual);
    if (newTimingInfo->horizontalBlanking != existingTimingInfo->horizontalBlanking)
        return (kDetailedTimingsNotEqual);
    if (newTimingInfo->verticalActive != existingTimingInfo->verticalActive)
        return (kDetailedTimingsNotEqual);
    if ((newTimingInfo->verticalBlanking & vMask) != (existingTimingInfo->verticalBlanking & vMask))
        return (kDetailedTimingsNotEqual);

    if (newTimingInfo->horizontalBorderLeft != existingTimingInfo->horizontalBorderLeft)
        return (kDetailedTimingsNotEqual);
    if (newTimingInfo->horizontalBorderRight != existingTimingInfo->horizontalBorderRight)
        return (kDetailedTimingsNotEqual);
    if ((newTimingInfo->verticalBorderTop & vMask) != (existingTimingInfo->verticalBorderTop & vMask))
        return (kDetailedTimingsNotEqual);
    if ((newTimingInfo->verticalBorderBottom & vMask) != (existingTimingInfo->verticalBorderBottom & vMask))
        return (kDetailedTimingsNotEqual);

    if (newTimingInfo->pixelClock == existingTimingInfo->pixelClock)
    {
        if (newTimingInfo->horizontalSyncOffset != existingTimingInfo->horizontalSyncOffset)
            return (kDetailedTimingsEqual);

        if (newTimingInfo->horizontalSyncPulseWidth != existingTimingInfo->horizontalSyncPulseWidth)
            return (kDetailedTimingsEqual);

        if ((newTimingInfo->verticalSyncOffset & vMask) != (existingTimingInfo->verticalSyncOffset & vMask))
            return (kDetailedTimingsEqual);

        if ((newTimingInfo->verticalSyncPulseWidth & vMask) != (existingTimingInfo->verticalSyncPulseWidth & vMask))
            return (kDetailedTimingsEqual);

        return (kDetailedTimingsIdentical);
    }

    return (kDetailedTimingsNotEqual);
}

static bool
GetTovr( IOFBConnectRef connectRef, IOAppleTimingID appleTimingID,  UInt32 * flags, UInt32 * _maskFlags )
{
    CFDictionaryRef tovr;
    CFDataRef       modetovr = NULL;
    UInt32          maskFlags = 0xffffffff;
    bool            result    = false;

    if (appleTimingID && connectRef->overrides)
    {
        if ((connectRef->defaultMinWidth == kInstallMinWidth)
         && (kDisplayVendorIDUnknown  == connectRef->displayVendor)
         && (kDisplayProductIDGeneric == connectRef->displayProduct))
        {
            if (kIOTimingIDVESA_1024x768_60hz == appleTimingID)
            {
                appleTimingID = kIOTimingIDVESA_800x600_60hz;
            }
            else if ((kIOTimingIDVESA_800x600_60hz == appleTimingID)
                        || (kIOTimingIDVESA_800x600_56hz == appleTimingID))
            {
                appleTimingID = kIOTimingIDVESA_1024x768_60hz;
            }
        }
        tovr = CFDictionaryGetValue( connectRef->overrides, CFSTR("tovr") );
        result = (tovr && (modetovr = CFDictionaryGetValue( tovr, (const void *) (uintptr_t) (UInt32) appleTimingID )));
        if (result)
        {
            DMTimingOverrideRec * tovrRec;
            tovrRec = (DMTimingOverrideRec *) CFDataGetBytePtr(modetovr);
            DEBG(connectRef, "tovr: clr %08x, set %08x\n", 
                OSReadBigInt32(&tovrRec->timingOverrideClearFlags, 0),
                OSReadBigInt32(&tovrRec->timingOverrideSetFlags, 0));
            maskFlags = ~OSReadBigInt32(&tovrRec->timingOverrideClearFlags, 0);
            *flags &= maskFlags;
            *flags |= OSReadBigInt32(&tovrRec->timingOverrideSetFlags, 0);
        }
        
    }

    if (_maskFlags)
        *_maskFlags = maskFlags;

    return (result);
}

static void
MergeDisplayModeInformation(IOFBConnectRef connectRef __unused, 
							IODisplayModeInformation * from, 
							IODisplayModeInformation * to)
{
	to->flags |= from->flags;
	if (!to->imageWidth)  to->imageWidth  = from->imageWidth;
	if (!to->imageHeight) to->imageHeight = from->imageHeight;
}

static void
IOFBSetImageSize(IOFBConnectRef connectRef __unused,
				 IOFBDisplayModeDescription * desc)
{
	float nativeWidth, nativeHeight;
	float width, height, aspectDiff;
	uint32_t imageWidth, imageHeight;

	DEBG(connectRef, "%d x %d, %d x %d, %d x %d\n",
				desc->timingInfo.detailedInfo.v2.horizontalActive,
				desc->timingInfo.detailedInfo.v2.verticalActive,
				desc->timingInfo.detailedInfo.v2.horizontalScaled,
				desc->timingInfo.detailedInfo.v2.verticalScaled,
				desc->info.imageWidth,
				desc->info.imageHeight
				);

	nativeWidth  = desc->timingInfo.detailedInfo.v2.horizontalActive;
	nativeHeight = desc->timingInfo.detailedInfo.v2.verticalActive;
	width = desc->timingInfo.detailedInfo.v2.horizontalScaled;
	if (!width) width = nativeWidth;
	height = desc->timingInfo.detailedInfo.v2.verticalScaled;
	if (!height) height = nativeHeight;
	if (kIOScaleSwapAxes & desc->timingInfo.detailedInfo.v2.scalerFlags)
	{
		aspectDiff = width;
		width = height;
		height = aspectDiff;
	}
	imageWidth  = desc->info.imageWidth;
	imageHeight = desc->info.imageHeight;
	if (!(kDisplayModeStretchedFlag & desc->info.flags))
	{
		aspectDiff = (nativeWidth / nativeHeight) / (width / height);
		if (aspectDiff > 1.03125)
			imageWidth = (width / height * imageHeight);
		else if (aspectDiff < (1.0 - 0.03125))
			imageHeight = (height / width * imageWidth);
	}
	if (kIOScaleSwapAxes & desc->timingInfo.detailedInfo.v2.scalerFlags)
	{
		desc->info.imageHeight = imageWidth;
		desc->info.imageWidth  = imageHeight;
	}
	else
	{
		desc->info.imageWidth  = imageWidth;
		desc->info.imageHeight = imageHeight;
	}

	DEBG(connectRef, "%d x %d\n", 
				desc->info.imageWidth,
				desc->info.imageHeight
				);
}

__private_extern__ kern_return_t
IOFBInstallMode( IOFBConnectRef connectRef, IODisplayModeID mode,
                 IOFBDisplayModeDescription * desc,
                 UInt32 driverFlags, IOOptionBits modeGenFlags )
{
    IOReturn                   ret = kIOReturnSuccess;
    CFMutableDictionaryRef     dict;
    CFMutableArrayRef          array;
    CFNumberRef                num;
    CFDataRef                  data;
    CFDataRef                  timingData = 0;
    IODisplayModeInformation * otherInfo;
    IODisplayModeInformation * info = &desc->info;
    IOTimingInformation *      timingInfo = &desc->timingInfo;


    array = (CFMutableArrayRef) CFDictionaryGetValue( connectRef->kernelInfo,
                                                       CFSTR(kIOFBDetailedTimingsKey) );

    if( timingInfo && (kIODetailedTimingValid & timingInfo->flags))
    {
        if(mode == -1)
        {
            timingInfo->detailedInfo.v2.detailedTimingModeID =
                kIODisplayModeIDReservedBase 
                + connectRef->arbModeIDSeed 
                + (array ? CFArrayGetCount(array) : 0);

            DEBG(connectRef, "arb mode %08x\n", (int) timingInfo->detailedInfo.v2.detailedTimingModeID);
        }
        else
            timingInfo->detailedInfo.v2.detailedTimingModeID = mode;

        timingData = CFDataCreate( kCFAllocatorDefault,
                                   (UInt8 *) &timingInfo->detailedInfo.v2,
                                   sizeof(IODetailedTimingInformationV2) );
    }

    if( connectRef->trimToDependent && info 
      && info->nominalWidth && info->nominalHeight) do {

        IOFBConnectRef  other;
        CFIndex         modeCount, i;
        CFDataRef       data;
        Boolean         matches;

        if( 0 == (info->flags & kDisplayModeSafetyFlags /*kDisplayModeSafeFlag*/))
            continue;

        other = connectRef->nextDependent;
        if( !other->modesArray)
            continue;
        modeCount = CFArrayGetCount( other->modesArray );
        if( !modeCount)
            continue;
    
        for( i = 0, matches = false;
             !matches && (i < modeCount);
             i++)  {
    
            dict = (CFMutableDictionaryRef) CFArrayGetValueAtIndex( other->modesArray, i );
            data = CFDictionaryGetValue( dict, CFSTR(kIOFBModeDMKey) );
            otherInfo = (IODisplayModeInformation *) CFDataGetBytePtr( data );
    
#define kNeedFlags (kDisplayModeValidFlag | kDisplayModeSafeFlag)
            if( kNeedFlags != (otherInfo->flags & kNeedFlags))
                continue;
            matches = (info->nominalWidth == otherInfo->nominalWidth)
                    && (info->nominalHeight == otherInfo->nominalHeight);
        }
        if( !matches)
            info->flags &= ~(kDisplayModeSafetyFlags /*kDisplayModeSafeFlag*/);

    } while( false );

    do
    {
        if( mode == -1)
        {
            // assign a programmable mode ID after checking for dups

            if( timingData && !(kIOFBScaledMode & modeGenFlags))
            {
                CFIndex modeCount, i;
                UInt32 eq = false;

                UInt32 maskFlags;

                GetTovr(connectRef, timingInfo->appleTimingID, &info->flags, &maskFlags);

                if (kIOFBEDIDDetailedMode & modeGenFlags)
                {
                    // add safe mode driver modes for dups
                    modeCount = (CFIndex) connectRef->driverModeCount;
                    for( i = 0; i < modeCount; i++)
                    {
                        if (kAddSafeFlags != (kAddSafeFlags & connectRef->driverModeInfo[i].info.flags))
                            continue;
                        if (DetailedTimingsEqual( &timingInfo->detailedInfo.v2,
                                              &connectRef->driverModeInfo[i].timingInfo.detailedInfo.v2,
                                              modeGenFlags))
                        {
							MergeDisplayModeInformation(connectRef, info, &connectRef->driverModeInfo[i].info);
                            connectRef->driverModeInfo[i].info.flags &= maskFlags;
                            eq = true;
                        }
                    }
                }
                if (eq)
                {
                    ret = kIOReturnPortExists;
                    continue;
                }

                // check driver modes for dups
                modeCount = (CFIndex) connectRef->driverModeCount;
                for( i = 0; i < modeCount; i++)
                {
                    if ((kDisplayModeBuiltInFlag | kDisplayModeNeverShowFlag /*| kDisplayModeInterlacedFlag*/)
                                & connectRef->driverModeInfo[i].info.flags)
                        continue;

                    if (InvalidTiming(connectRef, &connectRef->driverModeInfo[i].timingInfo))
                        continue;

                    if( (0 != (kIOInterlacedCEATiming & timingInfo->detailedInfo.v2.signalConfig))
                        != (0 != (kDisplayModeInterlacedFlag & connectRef->driverModeInfo[i].info.flags)))
                        continue;

                    // 2488698, 3052614
                    if ((kIOTimingIDApple_FixedRateLCD == connectRef->driverModeInfo[i].timingInfo.appleTimingID)
                     && !CFDictionaryGetValue(connectRef->overrides, CFSTR(kIODisplayIsDigitalKey)))
                        continue;

                    if ((kIOTimingIDInvalid != timingInfo->appleTimingID)
                     && (kIOTimingIDInvalid != connectRef->driverModeInfo[i].timingInfo.appleTimingID)
                     && (kIOTimingIDApple_FixedRateLCD != connectRef->driverModeInfo[i].timingInfo.appleTimingID))
                    {
                        if ((eq = (timingInfo->appleTimingID == connectRef->driverModeInfo[i].timingInfo.appleTimingID)))
                            break;
                        continue;
                    }

                    if (0 == (kIODetailedTimingValid & connectRef->driverModeInfo[i].timingInfo.flags))
                        continue;
                    if ((eq = DetailedTimingsEqual( &timingInfo->detailedInfo.v2,
                                              &connectRef->driverModeInfo[i].timingInfo.detailedInfo.v2,
                                              modeGenFlags)))
                        break;
                }

                if (eq)
                {
                    DEBG(connectRef, "%d(%x) has a driver mode(%d)\n", (int) timingInfo->appleTimingID, 
                                (int) modeGenFlags, (int) connectRef->driverModeInfo[i].timingInfo.appleTimingID);

                    if ((kDetailedTimingsIdentical != eq) && (kIOFBEDIDDetailedMode & modeGenFlags))
                    {
                        connectRef->driverModeInfo[i].info.flags = kDisplayModeNeverShowFlag;
                        DEBG(connectRef, "disabling\n");
                    }
                    else
                    {
                        if (0 == (kIOFBGTFMode & modeGenFlags))
                         /* && (!connectRef->overrides 
                                    || !CFDictionaryGetValue(connectRef->overrides, CFSTR("trng")))*/
                        {
							MergeDisplayModeInformation(connectRef, info, &connectRef->driverModeInfo[i].info);
                            connectRef->driverModeInfo[i].info.flags &= maskFlags;
                        }

                        ret = kIOReturnPortExists;
                        continue;
                    }
                }

                // check already added modes for dups
                modeCount = array ? CFArrayGetCount(array) : 0;
                for( i = connectRef->arbModeBase; i < modeCount; i++) {
    
                    data = CFArrayGetValueAtIndex(array, i);
                    if( !data)
                        continue;
                    if( DetailedTimingsEqual( &timingInfo->detailedInfo.v2,
                                              (IODetailedTimingInformationV2 *) CFDataGetBytePtr(data),
                                              modeGenFlags)) {
                        ret = kIOReturnPortExists;
                        break;
                    }
                }
                if( kIOReturnSuccess != ret)
                    continue;
            }

            // no dups
            if ((0 == (kIOFBScaledMode & modeGenFlags)) && !connectRef->fbRange)
            {
                ret = kIOReturnUnsupported;
                continue;
            }

            if( !array) {
                array = CFArrayCreateMutable( kCFAllocatorDefault, 0,
                                            &kCFTypeArrayCallBacks );
                if( !array) {
                    ret = kIOReturnNoMemory;
                    continue;
                }
                CFDictionarySetValue( connectRef->kernelInfo,
                                      CFSTR(kIOFBDetailedTimingsKey), array );
                CFRelease( array );
            }
            mode = timingInfo->detailedInfo.v2.detailedTimingModeID;
            if( timingData)
                CFArrayAppendValue( array, timingData );
        }
    
        if( NULL == info)
            continue;
    
        dict = (CFMutableDictionaryRef) CFDictionaryGetValue( connectRef->modes,
                                                            (const void *) (uintptr_t) (UInt32) mode );
        if( !dict) {
            dict = CFDictionaryCreateMutable( kCFAllocatorDefault, (CFIndex) 0,
                                                &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks );
            if( dict) {
                CFArrayAppendValue( connectRef->modesArray, dict );
                CFDictionarySetValue( connectRef->modes, (const void *) (uintptr_t) (UInt32) mode, dict );
                CFRelease( dict );
            } else {
                ret = kIOReturnNoMemory;
                continue;
            }
        }
    
        num = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &mode );
        if( num) {
            CFDictionarySetValue( dict, CFSTR(kIOFBModeIDKey), num );
            CFRelease( num );
        }
    
        if( driverFlags && (0 == (mode & 0x80000000))) {
            num = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &driverFlags );
            if( num) {
                CFDictionarySetValue( dict, CFSTR(kIOFBModeDFKey), num );
                CFRelease( num );
            }
        }
    
        if( info) {

            data = CFDataCreate( kCFAllocatorDefault,
                            (UInt8 *) info, sizeof(IODisplayModeInformation));
            if( data) {
                CFDictionarySetValue( dict, CFSTR(kIOFBModeDMKey), data );
                CFRelease(data);
            }
        }
    
        if( timingData)
            CFDictionaryAddValue( dict, CFSTR(kIOFBModeTMKey), timingData );    // add if not present

        if( timingInfo && timingInfo->appleTimingID) {
            num = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &timingInfo->appleTimingID );
            if( num) {
                CFDictionarySetValue( dict, CFSTR(kIOFBModeAIDKey), num );
                CFRelease( num );
            }
        }

    } while( false );
    
    if( timingData)
        CFRelease(timingData);

    return( ret );
}

static kern_return_t
IOFBSetKernelConfig( IOFBConnectRef connectRef )
{
    kern_return_t err = kIOReturnSuccess;

    DEBG(connectRef, "IOFBSetKernelConfig: %ld\n", CFDictionaryGetCount(connectRef->kernelInfo));

    if( CFDictionaryGetCount(connectRef->kernelInfo)) {
        err = IOConnectSetCFProperty( connectRef->connect, CFSTR(kIOFBConfigKey), connectRef->kernelInfo );
    }

    return( err );
}

static void 
IOFBDictSetValues(const void *key, const void *value, void *context)
{
    CFMutableDictionaryRef dict = context;
    CFDictionarySetValue(dict, key, value);
}

__private_extern__ kern_return_t
IOFBSetKernelDisplayConfig( IOFBConnectRef connectRef )
{
    kern_return_t err = kIOReturnSuccess;
    CFDataRef     data;
    enum {        kNumAttrPairs = 6 };
    uint32_t      attributes[kNumVendors * kNumAttrPairs * 2];
    uint32_t      value;
    uint32_t      attrIdx = 0;
    int32_t       idx;

    DEBG(connectRef, "IOFBSetKernelDisplayConfig\n");

	if (!connectRef->setKernelDisplayConfig) return (kIOReturnSuccess);
    connectRef->setKernelDisplayConfig = false;

    for (idx = (kNumVendors - 1); idx >= 0; idx--)
    {
        static const uint32_t vendors[kNumVendors] = { 0, 0x1002, 0x8086, 0x10de };

        if (!((1 << idx) & connectRef->vendorsFound))
            continue;

        // 1
        attributes[attrIdx++] = kConnectionVendorTag;
        attributes[attrIdx++] = vendors[idx];

        // 2
        attributes[attrIdx++] = kConnectionFlags;
        attributes[attrIdx++] = connectRef->ovrFlags;

        // 3
        attributes[attrIdx++] = kConnectionColorModesSupported;
        value = connectRef->supportedColorModes[idx];
        if ((kAllVendors != idx) && (kIODisplayColorModeReserved == value))
            value = connectRef->supportedColorModes[kAllVendors];
        attributes[attrIdx++] = value;

        // 4
        attributes[attrIdx++] = kConnectionColorDepthsSupported;
        value = connectRef->supportedComponentDepths[idx];
        if ((kAllVendors != idx) && (kIODisplayRGBColorComponentBitsUnknown == value))
            value = connectRef->supportedComponentDepths[kAllVendors];
        attributes[attrIdx++] = value;

        // 5
        attributes[attrIdx++] = kConnectionControllerDitherControl;
        value = connectRef->ditherControl[idx];
        if ((kAllVendors != idx) && (kIODisplayDitherControlDefault == value))
            value = connectRef->ditherControl[kAllVendors];
        attributes[attrIdx++] = value;

        // 6 kNumAttrPairs
        attributes[attrIdx++] = kConnectionDisplayFlags;
        value = 0;
        if (connectRef->addTVFlag)
            value |= kIODisplayNeedsCEAUnderscan;
        attributes[attrIdx++] = value;
    }

    data = CFDataCreate(kCFAllocatorDefault,
                        (UInt8 *) &attributes[0], 
                        attrIdx * sizeof(attributes[0]));

    if (data && connectRef->displayAttributes)
    {
        CFDictionarySetValue(connectRef->displayAttributes, CFSTR(kIODisplayAttributesKey), data);
        CFRelease(data);

		CFDictionaryRef attrDict;
		attrDict = CFDictionaryGetValue(connectRef->overrides, CFSTR(kIODisplayAttributesKey));
		if (attrDict && (CFDictionaryGetTypeID() == CFGetTypeID(attrDict)))
			CFDictionaryApplyFunction(attrDict, &IOFBDictSetValues, connectRef->displayAttributes);

        err = IOConnectSetCFProperty(connectRef->connect, CFSTR(kIODisplayAttributesKey), connectRef->displayAttributes);
//      CFDictionarySetValue(connectRef->kernelInfo, CFSTR(kIODisplayAttributesKey), connectRef->displayAttributes);
    }

    return( err );
}

static kern_return_t
IOFBBuildModeList( IOFBConnectRef connectRef, Boolean forConnectChange )
{
    kern_return_t               err;
    CFMutableDictionaryRef      dict;
    CFMutableArrayRef           array;
    CFDataRef                   data;
    CFDataRef                   scalerProp;
    CFDataRef                   timingProp = NULL;
    CFNumberRef                 num;
    IODisplayModeID *           modes;
    IOFBDisplayModeDescription *modeInfo;
    IOOptionBits *              driverFlags;
    IOFBDisplayModeDescription *arbModeInfo;
    UInt32                      i, modeCount = 0, arbModeCount;
    IODisplayModeID             mode;
    IOFBDisplayModeDescription  currentTiming;
    IODisplayModeID             currentMode = kIODisplayModeIDInvalid;
    IOIndex                     currentDepth;
	IOTimingInformation 		_startupTiming;
	IOTimingInformation * 		startupTiming = NULL;
    IOFBDisplayModeDescription * info;
    IOOptionBits                installedFlags;
    IOFBDisplayModeDescription  scaleDesc;
    Boolean                     scaleCandidate, scaleVGA, pruneKeepCurrent;

    if( connectRef->kernelInfo)
        CFRelease( connectRef->kernelInfo );
    if( connectRef->modes)
        CFRelease( connectRef->modes );
    if( connectRef->modesArray)
        CFRelease( connectRef->modesArray );

    connectRef->suppressRefresh    = (0 != connectRef->overrides);
    connectRef->detailedRefresh    = false;
    connectRef->useScalerUnderscan = false;
    connectRef->addTVFlag          = false;
    connectRef->matchMode          = kIODisplayModeIDInvalid;
    connectRef->startMode          = kIODisplayModeIDInvalid;

    dict = CFDictionaryCreateMutable( kCFAllocatorDefault, (CFIndex) 0,
                                             (CFDictionaryKeyCallBacks *) 0,
                                             &kCFTypeDictionaryValueCallBacks );
    connectRef->modes = dict;
    if (forConnectChange)
        dict = NULL;
    else
        dict = (CFMutableDictionaryRef) IORegistryEntryCreateCFProperty(
                                        connectRef->framebuffer, 
                                        CFSTR(kIOFBConfigKey),
                                        kCFAllocatorDefault, kNilOptions);

    if( true && dict && (array = (CFMutableArrayRef) CFDictionaryGetValue( dict, CFSTR(kIOFBModesKey)))) {
        // pick up existing config
        connectRef->kernelInfo = dict;
        CFRetain(array);
        connectRef->modesArray = array;

        if( connectRef->suppressRefresh)
            connectRef->suppressRefresh = (0 != CFDictionaryGetValue(dict, CFSTR("IOFB0Hz")));

        connectRef->detailedRefresh = (0 != CFDictionaryGetValue(dict, CFSTR("IOFBmHz")));
        connectRef->displayMirror = (0 != CFDictionaryGetValue(dict, CFSTR("IOFBmir")));
        connectRef->useScalerUnderscan = (0 != CFDictionaryGetValue(dict, CFSTR("IOFBScalerUnderscan")));
        connectRef->addTVFlag = (0 != CFDictionaryGetValue(dict, CFSTR("IOFBtv")));

        if( (data = CFDictionaryGetValue(dict, CFSTR("dims"))))
            bcopy( CFDataGetBytePtr(data), &connectRef->dimensions, sizeof(connectRef->dimensions) );

        modeCount = CFArrayGetCount( connectRef->modesArray );
        for( i = 0; i < modeCount; i++ ) {
            UInt32 key;

            dict = (CFMutableDictionaryRef) CFArrayGetValueAtIndex( connectRef->modesArray, i );
            num = CFDictionaryGetValue( dict, CFSTR(kIOFBModeIDKey) );
            CFNumberGetValue( num, kCFNumberSInt32Type, (SInt32 *) &key );
            CFDictionarySetValue( connectRef->modes, (const void *)(uintptr_t) (UInt32)key, dict );
        }

        connectRef->relaunch = true;

        return( kIOReturnSuccess );
    }

    dict = CFDictionaryCreateMutable( kCFAllocatorDefault, (CFIndex) 0,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks );
    connectRef->kernelInfo = dict;

    connectRef->modesArray = CFArrayCreateMutable( kCFAllocatorDefault, 0,
                                                   &kCFTypeArrayCallBacks );
    CFDictionarySetValue( dict, CFSTR(kIOFBModesKey), connectRef->modesArray );

    scalerProp = IORegistryEntryCreateCFProperty( connectRef->framebuffer, CFSTR(kIOFBScalerInfoKey),
                                                  kCFAllocatorDefault, kNilOptions );
    if (scalerProp && (size_t) CFDataGetLength(scalerProp) >= sizeof(IODisplayScalerInformation))
        connectRef->scalerInfo = (IODisplayScalerInformation *) CFDataGetBytePtr(scalerProp);

    if (connectRef->scalerInfo)
    {
        DEBG(connectRef, "FB scaler info: (%d x %d), features %08x\n",
                (int) connectRef->scalerInfo->maxHorizontalPixels, 
                (int) connectRef->scalerInfo->maxVerticalPixels, 
                (int) connectRef->scalerInfo->scalerFeatures);
    }

    // -- keep timing for alias mode

    connectRef->arbModeBase   = 0;
    connectRef->arbModeIDSeed = (0x00007000 & (connectRef->arbModeIDSeed + 0x00001000));
    DEBG(connectRef, "seed : 0x%08x\n", (int) connectRef->arbModeIDSeed);

    bzero(&currentTiming, sizeof(currentTiming));
    connectRef->matchMode          = kIODisplayModeIDInvalid;
    connectRef->startMode          = kIODisplayModeIDInvalid;

    do
    {
        CFMutableArrayRef array;
        CFDataRef         timingData;

        err = _IOFBGetCurrentDisplayModeAndDepth(connectRef, &currentMode, &currentDepth);
        if (((kIODisplayModeIDReservedBase | kIODisplayModeIDAliasBase) != currentMode) && !connectRef->inMuxSwitch)
            break;
        err = IOFBCreateDisplayModeInformation(connectRef, currentMode, &currentTiming);
        if (kIOReturnSuccess != err)
            break;
        if (!(kIODetailedTimingValid & currentTiming.timingInfo.flags))
            break;

        currentTiming.timingInfo.detailedInfo.v2.detailedTimingModeID = currentMode;
        timingData = CFDataCreate(kCFAllocatorDefault,
                                  (UInt8 *) &currentTiming.timingInfo.detailedInfo.v2,
                                  sizeof(IODetailedTimingInformationV2));
        if (!timingData)
            break;
        array = CFArrayCreateMutable(kCFAllocatorDefault, 0,
                                     &kCFTypeArrayCallBacks);
        if (!array)
            break;
        CFArrayAppendValue(array, timingData);
        CFRelease(timingData);
        CFDictionarySetValue(connectRef->kernelInfo,
                             CFSTR(kIOFBDetailedTimingsKey), array);
        CFRelease(array);
        connectRef->arbModeBase = 1;
        connectRef->inMuxSwitch = true;
    }
    while (false);

    // -- get the info for all driver modes

#if DEBUG_NO_DRIVER_MODES
    if (!connectRef->dependentIndex)
        err = _IOFBGetDisplayModeCount( connectRef, &modeCount );
    else
        err = kIOReturnUnsupported;
#else
    err = _IOFBGetDisplayModeCount( connectRef, &modeCount );
#endif
    if( kIOReturnSuccess == err) {
        modes       = (IODisplayModeID *) calloc(modeCount, sizeof(IODisplayModeID));
        modeInfo    = (IOFBDisplayModeDescription *) calloc(modeCount, sizeof(IOFBDisplayModeDescription));
        driverFlags = (IOOptionBits *) calloc(modeCount, sizeof(IOOptionBits));
        err         = _IOFBGetDisplayModes( connectRef, modeCount, modes );
    } else {
        modes       = 0;
        modeInfo    = 0;
        driverFlags = 0;
        modeCount   = 0;
    }

    connectRef->driverModeInfo  = modeInfo;
    connectRef->driverModeCount = modeCount;

    for( i = 0; i < modeCount; i++) {
        if( (modes[i] & kIODisplayModeIDReservedBase) && (modes[i] != kIOFBSWOfflineDisplayModeID) ) {
            err = kIOReturnBadArgument;
            DEBG(connectRef, "Driver is attempting to create a mode with the reserved base mode ID bit set!");
        }
        else
            err = IOFBCreateDisplayModeInformation( connectRef, modes[i], &modeInfo[i] );
        if( kIOReturnSuccess != err) {
            modes[i] = 0;
            continue;
        }
        driverFlags[i] = modeInfo[i].info.flags;

#if RLOG
        DEBG(connectRef, "driver mode[%d] (%x,%x) %d x %d %f Hz flags %x\n", 
             (int) i, (int) modes[i], (int) modeInfo[i].timingInfo.appleTimingID,
             (int) modeInfo[i].info.nominalWidth, (int) modeInfo[i].info.nominalHeight,
             modeInfo[i].info.refreshRate / 65536.0, (int) driverFlags[i]);
        IOFBLogTiming(connectRef, &modeInfo[i].timingInfo);
#endif
    }

    // -- get modes from display

    if (!(kIOFBConnectStateUnusable & connectRef->state)) {

        IODisplayInstallTimings( connectRef );

        if( (data = CFDictionaryGetValue( connectRef->overrides, CFSTR("dims")))) {
            bcopy( CFDataGetBytePtr(data), &connectRef->dimensions, sizeof(connectRef->dimensions) );
            CFRetain(data);
        } else
            data = CFDataCreate( kCFAllocatorDefault,
                                 (const UInt8 *) &connectRef->dimensions,
                                 sizeof(connectRef->dimensions) );
        if( data) {
            CFDictionarySetValue( dict, CFSTR("dims"), data );
            CFRelease(data);
        }
    }

    // -- gather all mode info

    array = (CFMutableArrayRef) CFDictionaryGetValue( connectRef->kernelInfo,
                                                        CFSTR(kIOFBDetailedTimingsKey) );
    arbModeCount = array ? (CFArrayGetCount(array) - connectRef->arbModeBase) : 0;
    arbModeInfo = (IOFBDisplayModeDescription *) 
        (arbModeCount
        ? (IOFBDisplayModeDescription *) calloc(arbModeCount, sizeof(IOFBDisplayModeDescription)) : 0);

    for( i = 0; i < (modeCount + arbModeCount); i++)
    {
        if( i >= modeCount)
        {
            CFDictionaryRef modeDict;

            info = &arbModeInfo[i - modeCount];

            data = CFArrayGetValueAtIndex(array, i - modeCount + connectRef->arbModeBase);
            CFDataGetBytes(data, CFRangeMake(0, sizeof(IODetailedTimingInformationV2)),
                            (UInt8 *) &info->timingInfo.detailedInfo.v2);
            info->timingInfo.flags = kIODetailedTimingValid;

            mode = info->timingInfo.detailedInfo.v2.detailedTimingModeID;

            modeDict = CFDictionaryGetValue( connectRef->modes, (const void *) (uintptr_t) (UInt32) mode );
            if (!modeDict)
            {
                DEBG(connectRef, "invalid mode 0x%x\n", (int) mode);
                continue;
            }
    
            if(!(data = CFDictionaryGetValue( modeDict, CFSTR(kIOFBModeDMKey) )))
            {
                DEBG(connectRef, "no kIOFBModeDMKey 0x%x\n", (int) mode);
                continue;
            }
            CFDataGetBytes(data, CFRangeMake(0, sizeof(IODisplayModeInformation)),
                            (UInt8 *) &info->info);

            if((num = CFDictionaryGetValue( modeDict, CFSTR(kIOFBModeAIDKey) )))
                CFNumberGetValue(num, kCFNumberSInt32Type, &info->timingInfo.appleTimingID);
        }
        else
        {
            mode = modes[i];
            if( 0 == mode)
                continue;
            info = &modeInfo[i];
        }

        IOFBAdjustDisplayModeInformation( connectRef, mode, info );
    }

    // -- refresh rate futzing

    for( i = 0; i < (modeCount + arbModeCount); i++)
    {
        UInt32  j;
        if( i >= modeCount)
            info = &arbModeInfo[i - modeCount];
        else if (!modes[i])
            continue;
        else
            info = &modeInfo[i];

        installedFlags = info->info.flags;
        if (!(kDisplayModeValidFlag & installedFlags))
            continue;

        MakeDetailedRefresh( connectRef, info );

        // make refresh rates unique
        for( j = 0; (!connectRef->detailedRefresh) && (j < i); j++ )
        {
            IOFBDisplayModeDescription * dupInfo;

            if (j >= modeCount)
                dupInfo = &arbModeInfo[j - modeCount];
            else if (!modes[j])
                continue;
            else
                dupInfo = &modeInfo[j];

            if( true
            && (((info->info.refreshRate + 0x8000) >> 16) 
                == ((dupInfo->info.refreshRate + 0x8000) >> 16))
            && (info->info.nominalWidth  == dupInfo->info.nominalWidth)
            && (info->info.nominalHeight == dupInfo->info.nominalHeight)
            && (0 == (~kDisplayModeSafetyFlags 
                & (info->info.flags ^ dupInfo->info.flags))) ) {

                connectRef->detailedRefresh = true;
            }
        }

        if (connectRef->suppressRefresh)
        {
            if ((kDisplayModeTelevisionFlag | kDisplayModeInterlacedFlag) & installedFlags)
                connectRef->suppressRefresh = false;
            else if(info->info.refreshRate
             && ((info->info.refreshRate < 0x398000) || (info->info.refreshRate > 0x3e8000)))
                connectRef->suppressRefresh = false;
        }
    }

    // -- install modes, look for scale candidate

    bzero( &scaleDesc, sizeof(scaleDesc) );
    scaleCandidate = scaleVGA = false;
    pruneKeepCurrent = false; //(0 != (kIOFBConnectStateOnline & connectRef->state));

    for( i = 0; i < (modeCount + arbModeCount); i++)
    {
        if( i >= modeCount)
        {
            info = &arbModeInfo[i - modeCount];
            mode = info->timingInfo.detailedInfo.v2.detailedTimingModeID;
            installedFlags = info->info.flags;
        }
        else
        {
            mode = modes[i];
            if( 0 == mode)
                continue;
            info = &modeInfo[i];
            installedFlags = driverFlags[i];
        }
        if (kDisplayModeValidFlag & info->info.flags)
            pruneKeepCurrent = false;

        if (connectRef->scalerInfo && !(kIOFBConnectStateUnusable & connectRef->state))
	    {
			Boolean ok = IOFBLookScaleBaseMode(connectRef, info, &scaleDesc);
         	if (CFDictionaryGetValue(connectRef->overrides, CFSTR(kIODisplayIsDigitalKey))) scaleCandidate |= ok;
	        else scaleVGA |= ok;
		}

		IOFBSetImageSize(connectRef, info);

        IOFBInstallMode( connectRef, mode, info,
                         installedFlags, kNilOptions );

    }

    if ( !(kIOFBConnectStateUnusable & connectRef->state)
        &&      ((kIOScaleRotateFlags & connectRef->transform) 
                 || (connectRef->useScalerUnderscan && (kIOFBScalerUnderscan & connectRef->transform))) )
    {
        for( i = 0; i < (modeCount + arbModeCount); i++)
        {
            // add the transformed version of driver modes
            IOFBDisplayModeDescription _desc;
            IOFBDisplayModeDescription * desc = &_desc;
    
            if( i >= modeCount)
            {
                info = &arbModeInfo[i - modeCount];
                mode = info->timingInfo.detailedInfo.v2.detailedTimingModeID;
                installedFlags = info->info.flags;
            }
            else
            {
                mode = modes[i];
                if( 0 == mode)
                    continue;
                info = &modeInfo[i];
                if (!(kIODetailedTimingValid & info->timingInfo.flags))
                    continue;
                installedFlags = driverFlags[i];
                if (!scaleCandidate)
                {
                    *desc = *info;
                    UpdateTimingInfoForTransform(connectRef, desc, kScaleInstallAlways);
			
                    err = IOFBInstallScaledMode( connectRef, desc, kScaleInstallAlways );
                    if (kIOReturnSuccess != err)
                        continue;
                }
            }

            if (!scaleCandidate && (kIOScaleSwapAxes & connectRef->transform))
            {
                UInt32 h, v;

                h = info->timingInfo.detailedInfo.v2.horizontalActive;
                v = info->timingInfo.detailedInfo.v2.verticalActive;

                if ((h == 1024) && (v == 768))
                {
                    h = 640;
                    v = 480;
                }
                else if ((h == 1280) && (v == 1024))
                {
                    h = 800;
                    v = 600;
                }
                else if ((h == 1600) && (v == 1200))
                {
                    h = 1280;
                    v = 1024;
                }
                else
                    h = v = 0;

                if (h && v)
                {
                    *desc = *info;
                    UpdateTimingInfoForTransform(connectRef, desc, kScaleInstallAlways);

                    desc->timingInfo.detailedInfo.v2.horizontalScaled = h;
                    desc->timingInfo.detailedInfo.v2.verticalScaled   = v;
            
                    err = IOFBInstallScaledMode(connectRef, desc, kScaleInstallAlways);

                    if ((h == 800) && (v == 600))
                    {
                        h = 1024;
                        v = 768;
                        desc->timingInfo.detailedInfo.v2.horizontalScaled = h;
                        desc->timingInfo.detailedInfo.v2.verticalScaled   = v;
                
                        err = IOFBInstallScaledMode(connectRef, desc, kScaleInstallAlways);
                    }
                }
            }

            if (i < modeCount)
            {
                // disable the driver mode
                info->info.flags &= ~kDisplayModeSafetyFlags;
                IOFBInstallMode( connectRef, mode, info, 0, kNilOptions );
            }
        }
    }

    if( modes)
        free( modes );
    if( modeInfo)
        free( modeInfo );
    if( driverFlags)
        free( driverFlags );
    if( arbModeInfo)
        free( arbModeInfo );
    modes       = 0;
    modeInfo    = 0;
    driverFlags = 0;
    arbModeInfo = 0;
    connectRef->driverModeInfo  = 0;
    connectRef->driverModeCount = 0;

    // -- scaling
    if(scaleCandidate || scaleVGA)
        IOFBInstallScaledModes( connectRef, &scaleDesc, scaleVGA );

    if( connectRef->suppressRefresh)
        CFDictionarySetValue(connectRef->kernelInfo, CFSTR("IOFB0Hz"), kCFBooleanTrue);
    if( connectRef->detailedRefresh)
        CFDictionarySetValue(connectRef->kernelInfo, CFSTR("IOFBmHz"), kCFBooleanTrue);
    if( connectRef->displayMirror)
        CFDictionarySetValue(connectRef->kernelInfo, CFSTR("IOFBmir"), kCFBooleanTrue);

    if (connectRef->useScalerUnderscan)
        CFDictionarySetValue(connectRef->kernelInfo, CFSTR("IOFBScalerUnderscan"), kCFBooleanTrue);
    if (connectRef->addTVFlag)
        CFDictionarySetValue(connectRef->kernelInfo, CFSTR("IOFBtv"), kCFBooleanTrue);

    // -- prune, search alias

    if( !(kIOFBConnectStateUnusable & connectRef->state)
        && (kIODisplayModeIDReservedBase | kIODisplayModeIDAliasBase) != currentMode)
    {
        timingProp = IORegistryEntryCreateCFProperty(connectRef->framebuffer, CFSTR(kIOFBStartupTimingPrefsKey),
                                                      kCFAllocatorDefault, kNilOptions );
        if (timingProp && (size_t) CFDataGetLength(timingProp) >= sizeof(typeof(_startupTiming)))
        {

            CFDataGetBytes(timingProp, CFRangeMake(0, sizeof(_startupTiming)),
                            (UInt8 *) &_startupTiming);

            startupTiming = &_startupTiming;
            IOFBTimingSanity(startupTiming);
#if RLOG
            DEBG(connectRef, " look startup mode \n");
            IOFBLogTiming(connectRef, startupTiming);
#endif
        }
        if (timingProp)
            CFRelease(timingProp);
    }

    modeCount = CFArrayGetCount( connectRef->modesArray );
    for( i = 0; i < modeCount; i++)
    {
        IODetailedTimingInformationV2 * timing;
        IODisplayModeInformation * dmInfo;
        CFNumberRef num;

        dict = (CFMutableDictionaryRef) CFArrayGetValueAtIndex( connectRef->modesArray, i );

        num = CFDictionaryGetValue( dict, CFSTR(kIOFBModeIDKey) );
        if( !num)
            continue;
        CFNumberGetValue( num, kCFNumberSInt32Type, &mode );
        if (pruneKeepCurrent && (mode == currentMode))
            continue;

        data = CFDictionaryGetValue( dict, CFSTR(kIOFBModeDMKey) );
        if( !data)
            continue;
        dmInfo = (IODisplayModeInformation *) CFDataGetBytePtr( data );

        if (!(dmInfo->flags & kDisplayModeValidFlag))
        {
            // remove it
            CFArrayRemoveValueAtIndex( connectRef->modesArray, i );
            CFDictionaryRemoveValue( connectRef->modes, (const void *) (uintptr_t) (UInt32) mode );
            i--; modeCount--;
            continue;
        }

        data = CFDictionaryGetValue(dict, CFSTR(kIOFBModeTMKey));
        if (!data)
            continue;
        timing = (typeof(timing)) CFDataGetBytePtr(data);

        if (connectRef->inMuxSwitch)
        {
            if (DetailedTimingsEqual(timing, 
                                    &currentTiming.timingInfo.detailedInfo.v2, 
                                    kNilOptions))
            {
                connectRef->matchMode  = (kIODisplayModeIDAliasBase | timing->detailedTimingModeID);
                connectRef->matchDepth = currentDepth;
                DEBG(connectRef, " swizzle -> %x\n", (int) connectRef->matchMode);
            }
        }
        else if (startupTiming
                 && (DetailedTimingsEqual(timing, 
                                            &startupTiming->detailedInfo.v2, 
                                            1*kIOFBTimingMatch)))
        {
            connectRef->startMode = mode;
            connectRef->startDepth = -1;
            DEBG(connectRef, " startup mode from timing %x\n", (int) mode);
        }
    }

    // -- install
	
	enum { kNeed = (kIOFBConnectStateUnusable | kIOFBConnectStateHWOnline) };
    if ((kNeed != (kNeed & connectRef->state))
     && CFArrayGetCount(connectRef->modesArray))
    {
        err = IOFBSetKernelConfig( connectRef );
	}

    if (scalerProp)
    {
        CFRelease(scalerProp);
        connectRef->scalerInfo = 0;
    }

    return( err );
}

static void
IOFBUpdateConnectState( IOFBConnectRef connectRef )
{
    connectRef->defaultMode    = 0;
    connectRef->defaultDepth   = 1;

    connectRef->displayVendor  = kDisplayVendorIDUnknown;
    connectRef->displayProduct = kDisplayProductIDGeneric;

    connectRef->state = IOFBGetState( connectRef );

	if (!(kIOFBConnectStateOnline & connectRef->state)) connectRef->state |= kIOFBConnectStateUnusable;
}

IOReturn
IOAccelReadFramebuffer(io_service_t framebuffer, uint32_t width __unused, uint32_t height, size_t rowBytes,
                        mach_vm_address_t * result, mach_vm_size_t * bytecount)
{
    IOReturn          err;
    mach_vm_size_t    size = 0;
    mach_vm_address_t buffer = 0;

    *result    = 0;
    *bytecount = 0;
	size = rowBytes * height;

	err = mach_vm_allocate(mach_task_self(), &buffer, size, 
						   VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_COREGRAPHICS_FRAMEBUFFERS));

    if (kIOReturnSuccess == err) 
    {
        *result    = buffer;
        *bytecount = size;
    }

    return (err);
}
static kern_return_t
IOFBResetTransform( IOFBConnectRef connectRef )
{
    kern_return_t err = kIOReturnSuccess;
    CFNumberRef   num;

    num = IORegistryEntryCreateCFProperty( connectRef->framebuffer, CFSTR(kIOFBTransformKey),
                                            kCFAllocatorDefault, kNilOptions );
    if( num) 
    {
        CFNumberGetValue( num, kCFNumberSInt64Type, &connectRef->transform );
        CFRelease(num);
    }
    else
        connectRef->transform = 0;

    if (connectRef->transformSurface)
    {
        IOAccelDestroySurface(connectRef->transformSurface);
        connectRef->transformSurface = 0;
    }

    DEBG(connectRef, " %qx\n", connectRef->transform);

    if (kIOFBRotateFlags & connectRef->transform) do
    {
        io_service_t                    accelerator;
        UInt32                          index;
        IOAccelDeviceRegion             rgn;
        IODisplayModeID                 mode;
        IOIndex                         depth;
        IOPixelInformation              pixelInfo;
        UInt32                          surfaceMode;
        IOAccelSurfaceInformation       surfaceInfo;
        UInt32                          vramSave;

        err = IOFBGetAttributeForFramebuffer( connectRef->connect,
                                                MACH_PORT_NULL,
                                                kIOVRAMSaveAttribute, &vramSave );
        DEBG(connectRef, "IOFBGetAttributeForFramebuffer(kIOVRAMSaveAttribute, %x), %08x\n", err, (int) vramSave);
        if (kIOReturnSuccess != err)
            vramSave = true;
        if (!vramSave)
            continue;

        err = _IOFBGetCurrentDisplayModeAndDepth(connectRef, &mode, &depth);
        DEBG(connectRef, "IOFBGetCurrentDisplayModeAndDepth(%x), %x, %d\n", err, (int) mode, (int) depth);
        if (err)
            continue;
        err = _IOFBGetPixelInformation( connectRef, mode, depth,
                                        kIOFBSystemAperture, &pixelInfo );
        DEBG(connectRef, "_IOFBGetPixelInformation(%x)\n", err);
        if (err)
            continue;

        rgn.num_rects = 0;
        rgn.bounds.x  = 0;
        rgn.bounds.y  = 0;
        rgn.bounds.w  = pixelInfo.activeWidth;
        rgn.bounds.h  = pixelInfo.activeHeight;

        surfaceMode = 0x00000040 /*| kIOAccelSurfaceModeWindowedBit*/;
        if (pixelInfo.bitsPerPixel == 32)
            surfaceMode |= kIOAccelSurfaceModeColorDepth8888;
        else
            surfaceMode |= kIOAccelSurfaceModeColorDepth1555;

        err = IOAccelFindAccelerator(connectRef->framebuffer, &accelerator, &index);
        DEBG(connectRef, "IOAccelFindAccelerator(%x)\n", err);
        if (err)
            continue;

        err = IOAccelCreateSurface(accelerator, index, surfaceMode, &connectRef->transformSurface);
        DEBG(connectRef, "IOAccelCreateSurface(%x)\n", err);
        if (err)
            continue;

        err = IOAccelSetSurfaceFramebufferShape(connectRef->transformSurface, &rgn, kNilOptions, index);
        DEBG(connectRef, "IOAccelSetSurfaceFramebufferShape(%x)\n", err);
        if (err)
            continue;

        err = IOAccelWriteLockSurface(connectRef->transformSurface, &surfaceInfo, sizeof(surfaceInfo));
        DEBG(connectRef, "IOAccelWriteLockSurface(%x)\n", err);
        if (err)
            continue;

    }
    while (false);

    if ((kIOReturnSuccess != err) && connectRef->transformSurface)
    {
        IOAccelDestroySurface(connectRef->transformSurface);
        connectRef->transformSurface = 0;
    }

    return (err);
}

static bool
IOFBWritePrefs( IOFBConnectRef connectRef )
{
    CFMutableDictionaryRef prefs, newPrefs;
    bool madeChanges = false;

    prefs = (CFMutableDictionaryRef) CFDictionaryGetValue(connectRef->iographicsProperties, CFSTR("prefs"));
    if (!prefs || !gIOGraphicsPrefsService)
        return (false);

    newPrefs = (CFMutableDictionaryRef) IORegistryEntryCreateCFProperty(gIOGraphicsPrefsService,
                                                CFSTR(kIOGraphicsPrefsKey),
                                                kCFAllocatorDefault, kNilOptions);
    madeChanges |= (!newPrefs || !CFEqual(newPrefs, prefs));
    if (newPrefs)
        prefs = newPrefs;

    DEBG(connectRef, "writePlist %d\n", madeChanges);
    if (madeChanges)
    {
        uid_t euid = geteuid();
        seteuid(0);
        writePlist(kIOFirstBootFlagPath, prefs, 0);
        seteuid(euid);
        CFDictionarySetValue(connectRef->iographicsProperties, CFSTR("prefs"), prefs);
    }
    if (newPrefs)
        CFRelease(newPrefs);

    return (true);
}

static kern_return_t
IOFBRebuild( IOFBConnectRef connectRef, Boolean forConnectChange )
{
    /*
    TIMESTART();
    
    if( kIOReturnSuccess != IOFBGetAttributeForFramebuffer( connectRef->connect, MACH_PORT_NULL,
                                    kIOMirrorDefaultAttribute, &connectRef->mirrorDefaultFlags))
        connectRef->mirrorDefaultFlags = 0;

    TIMEEND("IOFBGetAttributeForFramebuffer");

    DEBG(connectRef, "IOFBRebuild(%d)\n", forConnectChange);

    DEBG(connectRef, "%p: ID(%qx,%d) -> %p, %08x, %08x, %08x\n",
            connectRef, connectRef->dependentID, (int) connectRef->dependentIndex, connectRef->nextDependent,
            (int) connectRef->state, connectRef->nextDependent ? (int) connectRef->nextDependent->state : 0,
            (int) connectRef->mirrorDefaultFlags);

    connectRef->trimToDependent  = (kIOMirrorForced == ((kIOMirrorForced | kIOMirrorNoTrim)
                                                    & connectRef->mirrorDefaultFlags))
                                && (0 != connectRef->dependentIndex)
                                && (connectRef->nextDependent)
                                && (0 != (kIOMirrorHint & connectRef->mirrorDefaultFlags));

    connectRef->defaultToDependent = true 
                                && (kIOMirrorForced == ((kIOMirrorForced | kIOMirrorNoTrim)
                                                    & connectRef->mirrorDefaultFlags))
                                && (0 != connectRef->dependentIndex)
                                && (connectRef->nextDependent)
                                && (0 != (kIOMirrorHint & connectRef->mirrorDefaultFlags));

    connectRef->dimensions.width      = 0xffffffff;
    connectRef->dimensions.height     = 0xffffffff;
    connectRef->dimensions.setFlags   = 0;
    connectRef->dimensions.clearFlags = 0;

    connectRef->defaultWidth          = 0;
    connectRef->defaultHeight         = 0;
    connectRef->defaultImageWidth     = 0;
    connectRef->defaultImageHeight    = 0;
    connectRef->displayImageWidth     = 0;
    connectRef->displayImageHeight    = 0;

    connectRef->displayMirror         = false;

    TIMESTART();
    
    IOFBCreateOverrides( connectRef );

    TIMEEND("IOFBCreateOverrides");

    if(forConnectChange && connectRef->overrides && !(kIOFBConnectStateUnusable & connectRef->state)) do
    {
        CFNumberRef num;
        SInt32 h = -1, v = -1;

        if( (num = CFDictionaryGetValue( connectRef->overrides, CFSTR(kDisplayHorizontalImageSize) )))
            CFNumberGetValue( num, kCFNumberSInt32Type, &h );
        if( (num = CFDictionaryGetValue( connectRef->overrides, CFSTR(kDisplayVerticalImageSize) )))
            CFNumberGetValue( num, kCFNumberSInt32Type, &v );

        if ((!h && !v) || (kDisplayVendorIDUnknown == connectRef->displayVendor))
            connectRef->displayMirror = true;

    } while( false );

    TIMESTART();

    IOFBResetTransform( connectRef );

    TIMEEND("IOFBResetTransform");

    TIMESTART();

    IOFBBuildModeList( connectRef, forConnectChange );

    if (0 == CFArrayGetCount(connectRef->modesArray))
    {
		DEBG(connectRef, " made unusable\n");
        connectRef->state |= kIOFBConnectStateUnusable;
        IOFBBuildModeList( connectRef, true );
    }

    TIMEEND("IOFBBuildModeList");

    TIMESTART();

    IOFBLookDefaultDisplayMode( connectRef );

    TIMEEND("IOFBLookDefaultDisplayMode");

    CFMutableDictionaryRef prefs;
    CFMutableDictionaryRef displayPrefs = NULL;
    CFTypeRef displayKey;
    displayKey = CFDictionaryGetValue(connectRef->overrides, CFSTR(kIODisplayPrefKeyKey));
    prefs = (CFMutableDictionaryRef) CFDictionaryGetValue(connectRef->iographicsProperties, CFSTR("prefs"));
    connectRef->firstBoot = (displayKey && (!prefs 
    			|| (NULL == (displayPrefs = (CFMutableDictionaryRef) CFDictionaryGetValue(prefs, displayKey)))));
    DEBG(connectRef, "firstBoot == %d\n", connectRef->firstBoot);

    connectRef->make4By3         = false
                                && (kIOMirrorDefault & connectRef->mirrorDefaultFlags)
                                && (connectRef->defaultNot4By3)
                                && (0 == connectRef->dependentIndex)
                                && (0 != (kIOMirrorHint & connectRef->mirrorDefaultFlags));

    bool logDisplay = false;
    if (kIOFBConnectStateHWOnline & connectRef->state)
    {
	logDisplay = connectRef->firstBoot;
	if (!logDisplay && displayPrefs)
	{
	    int32_t version = 0;
            CFNumberRef
	    num = CFDictionaryGetValue(displayPrefs, CFSTR(kIOGraphicsPrefsVersionKey));
	    if (num && (CFGetTypeID(num) == CFNumberGetTypeID()))
	    {
		CFNumberGetValue(num, kCFNumberSInt32Type, &version);
	    }
	    logDisplay = (version < 2);
    	}
    }

    if (logDisplay)
    {
	aslmsg (msg) = asl_new(ASL_TYPE_MSG);
	if (msg)
	{
	    char                sbuf[256];
	    CFDataRef           edidData;
	    io_registry_entry_t regEntry;

	    snprintf(sbuf, sizeof(sbuf), "0x%x,0x%x(%dx%d)", 
			    (int)connectRef->displayVendor, (int)connectRef->displayProduct,
			    (int)connectRef->defaultWidth, (int)connectRef->defaultHeight);

	    asl_set(msg, kMsgTracerKeyDomain,    "com.apple.iokit.graphics.displaytype" );
	    asl_set(msg, kMsgTracerKeySignature, sbuf);

	    if ((edidData = CFDictionaryGetValue(connectRef->overrides, CFSTR(kIODisplayEDIDKey)))
	      && IODisplayEDIDName((EDID *) CFDataGetBytePtr(edidData), sbuf))
	    {
		asl_set(msg, kMsgTracerKeySignature2, sbuf);
	    }
	    regEntry = IORegistryEntryFromPath(kIOMasterPortDefault, 
                                               kIOServicePlane ":/");
	    if (regEntry)
	    {
	    	if (kIOReturnSuccess == IORegistryEntryGetName(regEntry, sbuf))
		{
		    asl_set(msg, kMsgTracerKeySignature3, sbuf);
		}
	    	IOObjectRelease(regEntry);
	    }
	    asl_set(msg, kMsgTracerKeyResult,	 "noop");
	    asl_log(NULL, msg, ASL_LEVEL_NOTICE, "displayonline");
	    asl_free(msg);
	}
    }
    */

    return( kIOReturnSuccess );
}


static void
IOFBProcessConnectChange( IOFBConnectRef connectRef )
{
    IOReturn                    err;
    IODisplayModeID             mode = 0;
    IOIndex                     depth = -1;
    IODisplayModeInformation    info;

#if RLOG
    if (gAllConnects)
    {
        gAllConnects->time0 = mach_absolute_time();
        if (gAllConnects->next)
            gAllConnects->next->time0 = gAllConnects->time0;
    }
#endif

    DEBG(connectRef, "IOFBProcessConnectChange\n");
    
    if (connectRef->matchMode != kIODisplayModeIDInvalid)
    {
        mode  = connectRef->matchMode;
        depth = connectRef->matchDepth;
    }
    else if (connectRef->startMode != kIODisplayModeIDInvalid)
    {
        mode  = connectRef->startMode;
        depth = connectRef->startDepth;
    }
    if (!mode)
        mode  = connectRef->defaultMode;
    if (-1 == depth)
        depth = connectRef->defaultDepth;

    if( connectRef->make4By3 && connectRef->default4By3Mode) {
        err = IOFBGetDisplayModeInformation( connectRef->connect, mode, &info );
        if( (kIOReturnSuccess == err) 
            && ratioOver(((float)info.nominalWidth) / ((float)info.nominalHeight), 4.0 / 3.0) > 1.03125) {
            mode = connectRef->default4By3Mode;
        }
    }

	enum { kNeed = (kIOFBConnectStateUnusable | kIOFBConnectStateHWOnline) };
	if (kNeed == (kNeed & connectRef->state))
	{
        __IOFBGetCurrentDisplayModeAndDepth(connectRef, &mode, &depth);
    }

    TIMESTART();

    DEBG(connectRef, " IOFBSetDisplayModeAndDepth 0x%x, 0x%x\n", (int)mode, depth);

    err = IOFBSetDisplayModeAndDepth( connectRef->connect, mode, depth );
    
    TIMEEND("IOFBSetDisplayModeAndDepth");
}

static void
IOFBInterestCallback( void * refcon, io_service_t service __unused,
                      natural_t messageType, void * messageArgument __unused )
{
    IOFBConnectRef connectRef = (IOFBConnectRef) refcon;
    IOFBConnectRef next;
    UInt32 value;
    
    switch( messageType) {

      case kIOMessageServiceIsSuspended:

	    DEBG(connectRef, " start connect change\n");

        next = connectRef;
        do {
            TIMESTART();

            _IOFBGetAttributeForFramebuffer( next->connect, MACH_PORT_NULL,
                                    kIOFBProcessConnectChangeAttribute, &value );

            TIMEEND("kIOFBProcessConnectChangeAttribute");
            
            next = next->nextDependent;

        } while( next && (next != connectRef) );

        next = connectRef;
        do {

			TIMESTART();
			IOFBUpdateConnectState( next );
			TIMEEND("IOFBUpdateConnectState");

			next->setKernelDisplayConfig = true;

			TIMESTART();
			DEBG(next, " IOFBRebuild1\n");
			IOFBRebuild( next, true );
			TIMEEND("IOFBRebuild");

            IOFBProcessConnectChange(next);
           
            next = next->nextDependent;

        } while( next && (next != connectRef) );
            
        next = connectRef;
        do {
            if (next->inMuxSwitch)  next->inMuxSwitch = false;
            else
			{
				TIMESTART();
				IOFBRebuild( next, true );
				TIMEEND("IOFBRebuild2");

				DEBG(next, " IOFBProcessConnectChange\n");
				IOFBProcessConnectChange(next);
			}
			next = next->nextDependent;

        } while( next && (next != connectRef) );

        next = connectRef;
        do {

            TIMESTART();

            if (next->clientCallbacks)
                next->clientCallbacks->ConnectionChange(next->clientCallbackRef, (void *) NULL);

            TIMEEND("ConnectionChange");

			enum { kNeed = (kIOFBConnectStateUnusable | kIOFBConnectStateOnline) };
			if (kNeed == (kNeed & next->state))
			{
				next->state &= ~kIOFBConnectStateOnline;
				if (next->clientCallbacks)
					next->clientCallbacks->ConnectionChange(next->clientCallbackRef, (void *) NULL);
			}
           
            next = next->nextDependent;
        } while( next && (next != connectRef) );
        
        TIMESTART();
        
        _IOFBGetAttributeForFramebuffer(connectRef->connect, MACH_PORT_NULL,
                                        kIOFBEndConnectChangeAttribute, &value);
        
        TIMEEND("kIOFBEndConnectChangeAttribute");
            
        break;

      case kIOMessageServicePropertyChange:
        IOFBWritePrefs(connectRef);
        break;

      default:
        break;
    }
}

mach_port_t
IOFBGetNotificationMachPort( io_connect_t connect )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if( connectRef)
        return( IONotificationPortGetMachPort( connectRef->notifyPort ));
    else
        return( MACH_PORT_NULL );
}

kern_return_t
IOFBDispatchMessageNotification( io_connect_t connect, mach_msg_header_t * message,
                                 UInt32 version, 
                                 const IOFBMessageCallbacks * callbacks, void * callbackRef )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );
    UInt32 value;

    switch( message->msgh_id)
    {
        case 0:
            connectRef->didPowerOff = true;
			if ((version >= kIOFBMessageCallbacksVersionCurrent) 
				&& callbacks->WillPowerOffWithImages)
			{
				callbacks->WillPowerOffWithImages(callbackRef, (void *) (uintptr_t) connect,
													arrayCnt(connectRef->imageBuffers),
													&connectRef->imageBuffers[0], &connectRef->imageSizes[0]);
			}
			else
			{
				callbacks->WillPowerOff(callbackRef, (void *) (uintptr_t) connect);
			}
            break;

        case 1:
            connectRef->didPowerOff = false;
            callbacks->DidPowerOn(callbackRef, (void *) (uintptr_t) connect);
            break;

        case 0x87654321:
            _IOFBGetAttributeForFramebuffer(connectRef->connect, MACH_PORT_NULL,
                                            kIOFBWSStartAttribute, &value);
    }

    connectRef->clientCallbacks   = callbacks;
    connectRef->clientCallbackRef = callbackRef;
    IODispatchCalloutFromMessage( NULL, message, connectRef->notifyPort );

    return( kIOReturnSuccess );
}

kern_return_t
IOFBAcknowledgeNotification( void * notificationID )
{
    io_connect_t connect = (io_connect_t) (uintptr_t) notificationID;

    if( connect)
        return( IOFBAcknowledgePM( connect ));
    else
        return( kIOReturnSuccess );
}

extern kern_return_t
IOFBAcknowledgePM( io_connect_t connect )
{
    IOFBConnectRef  connectRef = IOFBConnectToRef( connect );
    IOReturn        err;
    UInt32          vramSave;
    IODisplayModeID mode;
    IOIndex         depth;
    IOPixelInformation  pixelInfo;
	uint32_t            idx;    
	bool                didPowerOff;
	
	didPowerOff = connectRef->didPowerOff;
    if (didPowerOff)
    do
    {
        connectRef->didPowerOff = false;

		if (connectRef->imageBuffers[kIOPreviewImageIndexDesktop]) continue;

        if (kIOFBConnectStateUnusable & connectRef->state)
            continue;

        err = _IOFBGetAttributeForFramebuffer( connect,
                                                MACH_PORT_NULL,
                                                kIOVRAMSaveAttribute, &vramSave );
        if ((kIOReturnSuccess != err) || !vramSave)
            continue;

        err = _IOFBGetCurrentDisplayModeAndDepth(connectRef, &mode, &depth);
        if (err)
            continue;
        err = _IOFBGetPixelInformation( connectRef, mode, depth,
                                        kIOFBSystemAperture, &pixelInfo );
        if (err)
            continue;
		syslog(LOG_ERR, "no sleep images for WillPowerOffWithImages\n");
        err = IOAccelReadFramebuffer(connectRef->framebuffer,
                                     pixelInfo.activeWidth, pixelInfo.activeHeight,
                                     pixelInfo.bytesPerRow,
                                     &connectRef->imageBuffers[kIOPreviewImageIndexDesktop],
                                     &connectRef->imageSizes[kIOPreviewImageIndexDesktop]);
        if (err)
            continue;
#if 0
	{
    mach_vm_size_t      bytes;

	bytes = connectRef->imageSizes[kIOPreviewImageIndexDesktop];
	mach_vm_allocate(mach_task_self(), &connectRef->imageBuffers[kIOPreviewImageIndexLockScreen], bytes, 
		  VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_COREGRAPHICS_FRAMEBUFFERS));
	connectRef->imageSizes[kIOPreviewImageIndexLockScreen] = bytes;
	bytes >>= 2;
	while (--bytes) ((uint32_t *)connectRef->imageBuffers[kIOPreviewImageIndexLockScreen])[bytes] = 0xff00ff00;
	}
#endif
    }
    while (false);

    if (didPowerOff)
	{
		// save images to files
		struct stat stat_buf;
		if (0 == stat(kIOGraphicsLogfilePath, &stat_buf)) do
		{
			err = _IOFBGetCurrentDisplayModeAndDepth(connectRef, &mode, &depth);
			if (err) continue;
			err = _IOFBGetPixelInformation(connectRef, mode, depth,
										   kIOFBSystemAperture, &pixelInfo);
			if (err) continue;
			for (idx = 0; idx < kIOPreviewImageCount; idx++)
			{
				struct {
				   uint8_t  identLength;
				   uint8_t  colorMapType;
				   uint8_t  dataType;
				   uint8_t  colorMap[5];
				   uint16_t origin[2];
				   uint16_t width;
				   uint16_t height;
				   uint8_t  bitsPerPixel;
				   uint8_t  imageDesc;
				} hdr;
				FILE *    f;
				uint8_t * bits;
				uint32_t  y;

				if (!connectRef->imageBuffers[idx]) continue;

				bzero(&hdr, sizeof(hdr));
				hdr.dataType     = 2;
				hdr.width        = OSSwapHostToLittleInt16(pixelInfo.activeWidth);
				hdr.height       = OSSwapHostToLittleInt16(pixelInfo.activeHeight);
				hdr.bitsPerPixel = pixelInfo.bitsPerPixel;
				hdr.imageDesc    = (1<<5) | 8;
		
				f = fopen(gIOGraphicsImageFiles[idx], "w" /*"r+"*/);
				if (!f) continue;
				fwrite(&hdr, sizeof(hdr), 1, f);
				bits = (uint8_t *)(uintptr_t) connectRef->imageBuffers[idx];
				for (y = 0; y < pixelInfo.activeHeight; y++)
				{
					fwrite(bits, sizeof(uint32_t), hdr.width, f);
					bits += pixelInfo.bytesPerRow;
				}
				fclose(f);
			}
		}
		while (false);
	}

    err = IOConnectCallMethod(connect, 14,         // Index
                &connectRef->imageBuffers[0], 2 * arrayCnt(connectRef->imageBuffers),
                NULL, 0, // Input
                NULL, NULL, NULL, NULL);           // Output

    for (idx = 0; idx < arrayCnt(connectRef->imageBuffers); idx++)
    {
	if (!connectRef->imageBuffers[idx]) continue;
	mach_vm_deallocate(mach_task_self(), connectRef->imageBuffers[idx], connectRef->imageSizes[idx]);
	connectRef->imageBuffers[idx] = connectRef->imageSizes[idx] = 0;
    }

    return (err);
}

// Display mode information

static void
IOFBCreateOverrides( IOFBConnectRef connectRef )
{
    io_service_t                framebuffer = connectRef->framebuffer;
    CFDictionaryRef             oldOvr = 0;
    CFMutableDictionaryRef      newDict, ovr = 0;
    CFTypeRef                   obj;
    CFNumberRef                 num;

    if( connectRef->overrides) {
        CFRelease( connectRef->overrides );
        connectRef->overrides = NULL;
    }

    do {
       
        oldOvr = _IODisplayCreateInfoDictionary( connectRef, framebuffer, kIODisplayNoProductName );
        if( !oldOvr)
            continue;

        num = CFDictionaryGetValue( oldOvr, CFSTR("IOGFlags") );
        if( num)
            CFNumberGetValue( num, kCFNumberSInt32Type, (SInt32 *) &connectRef->ovrFlags );
        else
            connectRef->ovrFlags = 0;

        num = CFDictionaryGetValue( oldOvr, CFSTR(kDisplayVendorID) );
        if( num)
            CFNumberGetValue( num, kCFNumberSInt32Type, (SInt32 *) &connectRef->displayVendor );
        num = CFDictionaryGetValue( oldOvr, CFSTR(kDisplayProductID) );
        if( num)
            CFNumberGetValue( num, kCFNumberSInt32Type, (SInt32 *) &connectRef->displayProduct );

        ovr = CFDictionaryCreateMutable( kCFAllocatorDefault, (CFIndex) 0,
                                                &kCFTypeDictionaryKeyCallBacks,  
                                                &kCFTypeDictionaryValueCallBacks );
        if( !ovr)
            continue;

        if ((newDict = IOFBMakeIntegerKeys(CFDictionaryGetValue(oldOvr, CFSTR("tovr")), false)))
        {
            CFDictionarySetValue( ovr, CFSTR("tovr"), newDict );
            CFRelease( newDict );
        }
        if ((newDict = IOFBMakeIntegerKeys(CFDictionaryGetValue(oldOvr, CFSTR("tinf")), false)))
        {
            CFDictionarySetValue( ovr, CFSTR("tinf"), newDict );
            CFRelease( newDict );
        }

        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR(kIODisplayEDIDKey)) ))
            CFDictionarySetValue( ovr, CFSTR(kIODisplayEDIDKey), obj );

        if ((obj = CFDictionaryGetValue(oldOvr, CFSTR(kIODisplayPrefKeyKey))))
            CFDictionarySetValue( ovr, CFSTR(kIODisplayPrefKeyKey), obj );

        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR(kDisplayHorizontalImageSize)) ))
            CFDictionarySetValue( ovr, CFSTR(kDisplayHorizontalImageSize), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR(kDisplayVerticalImageSize)) ))
            CFDictionarySetValue( ovr, CFSTR(kDisplayVerticalImageSize), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR(kIODisplayIsDigitalKey)) ))
            CFDictionarySetValue( ovr, CFSTR(kIODisplayIsDigitalKey), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR(kDisplayFixedPixelFormat)) ))
            CFDictionarySetValue( ovr, CFSTR(kDisplayFixedPixelFormat), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR(kIODisplayAttributesKey)) ))
            CFDictionarySetValue( ovr, CFSTR(kIODisplayAttributesKey), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("trng")) ))
            CFDictionarySetValue( ovr, CFSTR("trng"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("drng")) ))
            CFDictionarySetValue( ovr, CFSTR("drng"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("dspc")) ))
            CFDictionarySetValue( ovr, CFSTR("dspc"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("mdes")) ))
            CFDictionarySetValue( ovr, CFSTR("mdes"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("dims")) ))
            CFDictionarySetValue( ovr, CFSTR("dims"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("sync")) ))
            CFDictionarySetValue( ovr, CFSTR("sync"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("scale-resolutions")) ))
            CFDictionarySetValue( ovr, CFSTR("scale-resolutions"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("scale-resolutions-4k")) ))
            CFDictionarySetValue( ovr, CFSTR("scale-resolutions-4k"), obj );
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("default-resolution")) ))
            CFDictionarySetValue( ovr, CFSTR("default-resolution"), obj );
        if( (obj = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &connectRef->ovrFlags ))) {
            CFDictionarySetValue( ovr, CFSTR("IOGFlags"), obj );
            CFRelease(obj);
        }
        if( (obj = CFDictionaryGetValue( oldOvr, CFSTR("DisplayPixelDimensions")) ))
            CFDictionarySetValue( ovr, CFSTR("DisplayPixelDimensions"), obj );

    } while( false );

    if( oldOvr)
        CFRelease( oldOvr );

    connectRef->overrides = ovr;
}

static IOIndex
IOFBIndexForPixelBits( IOFBConnectRef connectRef, IODisplayModeID mode,
                                      IOIndex maxIndex, UInt32 bpp )
{
    IOPixelInformation  pixelInfo;
    IOIndex             index, depth = -1;
    kern_return_t       err;

    for( index = 0; index <= maxIndex; index++ ) {

        err = _IOFBGetPixelInformation( connectRef, mode, index,
                                        kIOFBSystemAperture, &pixelInfo );
        if( (kIOReturnSuccess == err) && (pixelInfo.bitsPerPixel >= bpp)) {
            depth = index;
            break;
        }
    }

    return( depth );
}

static IOIndex
IOFBIndexForPixelFormat( IOFBConnectRef connectRef, IODisplayModeID mode,
                      IOIndex maxIndex, IOPixelEncoding pixelFormat )
{
    IOPixelInformation  pixelInfo;
    IOIndex             index, depth = -1;
    kern_return_t       err;
    
    for( index = 0; index <= maxIndex; index++ ) {
        
        err = _IOFBGetPixelInformation( connectRef, mode, index,
                                       kIOFBSystemAperture, &pixelInfo );
        if( (kIOReturnSuccess == err) && (!strcmp(pixelFormat, pixelInfo.pixelFormat))) {
            depth = index;
            break;
        }
    }
    
    return( depth );
}

static kern_return_t
IOFBLookDefaultDisplayMode( IOFBConnectRef connectRef )
{
    IOReturn                    err;
    CFDataRef                   data;
    CFIndex                     modeCount, i;
    SInt32                      bestDefault, rDefault;
    SInt32                      bestQuality, rQuality;
    CFDictionaryRef             dict;
    IODisplayModeID             mode, bestMode = 0;
    IODisplayModeInformation    bestInfo = { .flags = 0 };
    IODisplayModeInformation *  info;
    SInt32                      bestDepth, minDepth, otherDepth = 0;
    CFDictionaryRef             ovr, tinf;
    CFDataRef                   modetinf;
    CFNumberRef                 num;
    SInt32                      timingID;
    Boolean                     better, defaultToDependent;
    UInt32                      desireRefresh;
    UInt32                      biggest4By3;
    float                       desireHPix, desireVPix;

    ovr = connectRef->overrides;
    if( ovr)
        tinf = CFDictionaryGetValue( ovr, CFSTR("tinf") );
    else
        tinf = 0;

    desireHPix = desireVPix = 0;
    desireRefresh = (86 << 16);

    if( ovr 
     && !CFDictionaryGetValue( ovr, CFSTR(kDisplayFixedPixelFormat))
     && !CFDictionaryGetValue( ovr, CFSTR(kIODisplayIsDigitalKey))) {
        if( (num = CFDictionaryGetValue( ovr, CFSTR(kDisplayHorizontalImageSize) ))) {
            CFNumberGetValue( num, kCFNumberFloatType, &desireHPix );
            if( desireHPix)
                desireHPix = desireHPix / mmPerInch * desireDPI;
        } 
        if( (num = CFDictionaryGetValue( ovr, CFSTR(kDisplayVerticalImageSize) ))) {
            CFNumberGetValue( num, kCFNumberFloatType, &desireVPix );
            if( desireVPix)
                desireVPix = desireVPix / mmPerInch * desireDPI;
        }
    }

    if( ovr && (data = CFDictionaryGetValue( ovr, CFSTR("default-resolution") ))) {
        UInt32 * value = (UInt32 *) CFDataGetBytePtr((CFDataRef) data);
        desireHPix    = (float) OSReadBigInt32(&value[0], 0);
        desireVPix    = (float) OSReadBigInt32(&value[1], 0);
        desireRefresh =         OSReadBigInt32(&value[2], 0);
    }

    bestQuality = bestDefault = 0;
    bestDepth = 1;

    if (kIOScaleSwapAxes & connectRef->transform)
    {
        float swap = desireHPix;
        desireHPix  = desireVPix;
        desireVPix = swap;
    }

    defaultToDependent = false;
    if( connectRef->defaultToDependent) do {

        if( kIOReturnSuccess != _IOFBGetCurrentDisplayModeAndDepth( connectRef->nextDependent,
                                                                    &mode, &otherDepth ))
            continue;
        dict = CFDictionaryGetValue( connectRef->nextDependent->modes, (const void *) (uintptr_t) (UInt32) mode );
        if( dict && (data = CFDictionaryGetValue( dict, CFSTR(kIOFBModeDMKey) ))) {
            info = (IODisplayModeInformation *) CFDataGetBytePtr(data);
            desireHPix = info->nominalWidth;
            desireVPix = info->nominalHeight;
            defaultToDependent = true;
        }

    } while( false );
    
    biggest4By3 = 0;
    connectRef->default4By3Mode = 0;

    modeCount = CFArrayGetCount( connectRef->modesArray );
    for( i = 0; i < modeCount; i++)  {

        dict = CFArrayGetValueAtIndex( connectRef->modesArray, i );
        better = false;
        data = (CFDataRef) CFDictionaryGetValue( dict, CFSTR(kIOFBModeDMKey) );
        if (!data) continue;
        info = (IODisplayModeInformation *) CFDataGetBytePtr(data);

        num = CFDictionaryGetValue( dict, CFSTR(kIOFBModeIDKey) );
        if (!num) continue;
        CFNumberGetValue( num, kCFNumberSInt32Type, &mode );

        num = CFDictionaryGetValue( dict, CFSTR(kIOFBModeAIDKey) );
        if( num)
            CFNumberGetValue( num, kCFNumberSInt32Type, &timingID );
        else
            timingID = 0;

        if (mode == connectRef->startMode) {
            IOIndex currentDepth = IOFBIndexForPixelFormat(connectRef, mode, info->maxDepthIndex, connectRef->currentModePixelInfo.pixelFormat);
            if (currentDepth == -1) {
                currentDepth = IOFBIndexForPixelBits(connectRef, mode, info->maxDepthIndex, 32);
            }
            
            connectRef->startDepth = currentDepth;
        }

        if( 0 == (info->flags & kDisplayModeValidFlag)) continue;
        if (kMirrorOnlyFlags & info->flags)             continue;

        // make sure it does >= 16bpp
        minDepth = IOFBIndexForPixelBits( connectRef, mode, info->maxDepthIndex, 16);

        if( minDepth < 0)
            continue;
        if( defaultToDependent)
            minDepth = otherDepth;

        if( (info->flags & kDisplayModeSafeFlag)
         && (info->nominalWidth > biggest4By3)
         && (ratioOver(((float)info->nominalWidth) / ((float)info->nominalHeight), 4.0 / 3.0) <= 1.03125)) {
            biggest4By3 = info->nominalWidth;
            connectRef->default4By3Mode = mode;
        }

        if( timingID && tinf && !defaultToDependent
        && (modetinf = CFDictionaryGetValue( tinf, (const void *) (uintptr_t) (UInt32) timingID ))) {
            DMDisplayTimingInfoRec *    tinfRec;
            tinfRec = (DMDisplayTimingInfoRec *) CFDataGetBytePtr(modetinf);
            rQuality = OSReadBigInt32(&tinfRec->timingInfoRelativeQuality, 0);
            rDefault = OSReadBigInt32(&tinfRec->timingInfoRelativeDefault, 0);
        } else
            rQuality = rDefault = 0;

        if( (info->nominalWidth < connectRef->defaultMinWidth) || (info->nominalHeight < connectRef->defaultMinHeight))
            rDefault--;
        else if (!defaultToDependent && (0 != (info->flags & kDisplayModeDefaultFlag)))
        {
            rDefault++;
            if (mode & 0x80000000)
                rDefault++;
        }

        if( !bestMode
         || ((info->flags & kDisplayModeSafeFlag) && (0 == (bestInfo.flags & kDisplayModeSafeFlag))))
            better = true;
        else {
#if 1
            if( (!defaultToDependent)
            && (bestInfo.flags & kDisplayModeSafeFlag)
            && (0 == (info->flags & kDisplayModeSafeFlag)))
                continue;
#else
            if( 0 == (info->flags & kDisplayModeSafeFlag))
                continue;
#endif
            if( rDefault < bestDefault)
                continue;
            better = (rDefault > bestDefault);

            if( !better) {

                if( (info->nominalWidth == bestInfo.nominalWidth)
                        && (info->nominalHeight == bestInfo.nominalHeight)) {

                    if( defaultToDependent && (0 == (info->flags & kDisplayModeSafeFlag)) )
                        better = (info->refreshRate < (61 << 16))
                            && (info->refreshRate > bestInfo.refreshRate);
                    else {
                        better = (info->refreshRate < desireRefresh)
                            && ((info->refreshRate > bestInfo.refreshRate) 
                                || (bestInfo.refreshRate >= desireRefresh));
                    }

                } else {
                    if (desireHPix && desireVPix) {
                        SInt32 delta1, delta2;

                        delta1 = ((abs(info->nominalWidth - ((SInt32)desireHPix) ))
                                    + abs(info->nominalHeight - ((SInt32)desireVPix) ));
                        delta2 = (abs(bestInfo.nominalWidth - ((SInt32)desireHPix) )
                                    + abs(bestInfo.nominalHeight - ((SInt32)desireVPix) ));
                        better = (delta1 < delta2);
                    }
                    else
                    {
                    	better = ((info->nominalWidth * info->nominalHeight) 
                    			> (bestInfo.nominalWidth * bestInfo.nominalHeight));
                    }
                }
            }
        }

        if( better) {
            bestMode = mode;
            bestQuality = rQuality;
            bestDefault = rDefault;
            bestInfo = *info;
            bestDepth = minDepth;
        }
    }

    if( bestMode) {

        connectRef->defaultMode = bestMode;
        if( !defaultToDependent
          && (bestInfo.maxDepthIndex > bestDepth))
            bestDepth++;
        connectRef->defaultDepth = bestDepth;

        connectRef->defaultNot4By3 = (ratioOver(((float)bestInfo.nominalWidth) / ((float)bestInfo.nominalHeight), 4.0 / 3.0) > 1.03125);

        err = kIOReturnSuccess;
    } else
        err = _IOFBGetCurrentDisplayModeAndDepth( connectRef,
                    &connectRef->defaultMode, &connectRef->defaultDepth );

    DEBG(connectRef, " 0x%x, 0x%x\n", (int)connectRef->defaultMode, connectRef->defaultDepth);

    return( err );
}

kern_return_t
IOFBGetDefaultDisplayMode( io_connect_t connect,
        IODisplayModeID * displayMode, IOIndex * displayDepth )
{
    IOFBConnectRef connectRef;

    connectRef = IOFBConnectToRef( connect);
    if( !connectRef)
        return( kIOReturnBadArgument );

    *displayMode = connectRef->defaultMode;
    *displayDepth = connectRef->defaultDepth;

    return( kIOReturnSuccess );
}


static Boolean
IOFBCheckScaleDupMode( IOFBConnectRef connectRef, 
		       IOFBDisplayModeDescription * desc,
		       IOOptionBits installFlags )
{
    CFDictionaryRef            dict;
    CFDataRef                  data;
    CFIndex                    i, modeCount;
    IODisplayModeInformation * info;
    Boolean                    dup = false;

    if ((0 == (kMirrorOnlyFlags & desc->info.flags)) 
    	&& (kScaleInstallAlways & installFlags))    return (false);

    modeCount = CFArrayGetCount( connectRef->modesArray );

    for( i = 0; (i < modeCount) && !dup; i++ )
    {
        dict = CFArrayGetValueAtIndex( connectRef->modesArray, i );
        if( !dict) continue;
        data = (CFDataRef) CFDictionaryGetValue( dict, CFSTR(kIOFBModeDMKey) );
        if( !data) continue;
        info = (IODisplayModeInformation *) CFDataGetBytePtr(data);
    
        do
        {
            if (0 == (kDisplayModeValidFlag & info->flags))                   continue;
            if (kDisplayModeBuiltInFlag & info->flags)                        continue;

			if (kMirrorOnlyFlags & desc->info.flags)
			{
				if (info->nominalWidth  != desc->info.nominalWidth)  continue;
				if (info->nominalHeight != desc->info.nominalHeight) continue;
			}
			else
			{
				if (kDisplayModeStretchedFlag & (info->flags ^ desc->info.flags)) continue;
				if (info->nominalWidth < (desc->info.nominalWidth - 20))   continue;
				if (info->nominalWidth > (desc->info.nominalWidth + 20))   continue;
				if (info->nominalHeight < (desc->info.nominalHeight - 20)) continue;
				if (info->nominalHeight > (desc->info.nominalHeight + 20)) continue;
			}
            dup = true;
        }
        while( false );
    }

    return( dup );
}

static kern_return_t
IOFBInstallScaledMode( IOFBConnectRef connectRef,
                       IOFBDisplayModeDescription * _desc,
                       IOOptionBits installFlags)
{
    IOFBDisplayModeDescription * desc = _desc;
    IOFBDisplayModeDescription __desc;
    UInt32 insetH, insetV, width, height, swap;
    kern_return_t kr;

	__desc = *_desc;
	desc = &__desc;

    kr = IOFBDriverPreflight(connectRef, desc);

    if ((kIOReturnSuccess != kr) 
     && !(kIOScaleCanBorderInsetOnly & connectRef->scalerInfo->scalerFeatures))
    {
        insetH = desc->timingInfo.detailedInfo.v2.horizontalScaledInset;
        insetV = desc->timingInfo.detailedInfo.v2.verticalScaledInset;

        if (insetH || insetV)
        {
            width = desc->timingInfo.detailedInfo.v2.horizontalScaled;
            height = desc->timingInfo.detailedInfo.v2.verticalScaled;

            if (kIOScaleSwapAxes & desc->timingInfo.detailedInfo.v2.scalerFlags)
            {
                swap = width;
                width  = height;
                height = swap;
            }

            if ((width == desc->timingInfo.detailedInfo.v2.horizontalActive)
             && (height == desc->timingInfo.detailedInfo.v2.verticalActive))
            {
                if (kIOScaleSwapAxes & desc->timingInfo.detailedInfo.v2.scalerFlags)
                {
                    swap = insetH;
                    insetH  = insetV;
                    insetV = swap;
                }
                desc->timingInfo.detailedInfo.v2.horizontalScaled -= 2*insetH;
                desc->timingInfo.detailedInfo.v2.verticalScaled   -= 2*insetV;

                kr = IOFBDriverPreflight(connectRef, desc);
            }
        }
    }
    if (kIOReturnSuccess != kr)
        return (kr);

    if (IOFBCheckScaleDupMode( connectRef, desc, installFlags ))
        return( 9 );

    IOFBSetImageSize(connectRef, desc);

    return(IOFBInstallMode( connectRef, 0xffffffff, desc, 0, kIOFBScaledMode));
}

__private_extern__ void
UpdateTimingInfoForTransform(IOFBConnectRef connectRef, 
                                IOFBDisplayModeDescription * desc,
                                IOOptionBits flags )
{
    Boolean doUnderscan = (connectRef->useScalerUnderscan 
                            && (kIOFBScalerUnderscan & connectRef->transform));
    UInt32 width, height, swap;

    desc->timingInfo.detailedInfo.v2.scalerFlags &= ~kIOScaleRotateFlags;

    if (!(kIOScaleRotateFlags & connectRef->transform) && !doUnderscan)
        return;

    width = desc->timingInfo.detailedInfo.v2.horizontalScaled;
    if (!width)
        width = desc->timingInfo.detailedInfo.v2.horizontalActive;
    height = desc->timingInfo.detailedInfo.v2.verticalScaled;
    if (!height)
        height = desc->timingInfo.detailedInfo.v2.verticalActive;

    if ((kIOScaleSwapAxes & connectRef->transform)
     && !(kScaleInstallNoResTransform & flags))
    {
        swap = width;
        width  = height;
        height = swap;
    }
    desc->timingInfo.detailedInfo.v2.horizontalScaled = width;
    desc->timingInfo.detailedInfo.v2.verticalScaled   = height;
    if (doUnderscan)
    {
        width  = desc->timingInfo.detailedInfo.v2.horizontalActive;
        height = desc->timingInfo.detailedInfo.v2.verticalActive;
        width = (width  >> 4) & ~7;
        height = (height >> 4) & ~1;
        desc->timingInfo.detailedInfo.v2.horizontalScaledInset = width;
        desc->timingInfo.detailedInfo.v2.verticalScaledInset   = height;

        if (kIOScaleCanBorderInsetOnly & connectRef->scalerInfo->scalerFeatures)
        {
            if ((kIOScaleSwapAxes & connectRef->transform)
             && !(kScaleInstallNoResTransform & flags))
            {
                swap = width;
                width  = height;
                height = swap;
            }
            desc->timingInfo.detailedInfo.v2.horizontalScaled -= 2*width;
            desc->timingInfo.detailedInfo.v2.verticalScaled   -= 2*height;
        }
    }

    desc->timingInfo.detailedInfo.v2.scalerFlags |= (connectRef->transform & kIOScaleRotateFlags);

#if RLOG
    if (desc->timingInfo.detailedInfo.v2.horizontalScaledInset
     || desc->timingInfo.detailedInfo.v2.verticalScaledInset)
    {
        DEBG(connectRef, "using inset:\n");
        IOFBLogTiming(connectRef, &desc->timingInfo);
    }
#endif
}    

static int
_IOFBInstallScaledResolution( IOFBConnectRef connectRef,
                                IOFBDisplayModeDescription * baseDesc,
                                float nativeWidth, float nativeHeight,
                                float width, float height,
                                IOOptionBits flags,
                                uint32_t setModeFlags, uint32_t clrModeFlags )
{
    IOFBDisplayModeDescription newDesc;
    IOFBDisplayModeDescription * desc = &newDesc;
    UInt32      need = 0;
    float       aspectDiff;
    float       ratio;
    Boolean     okToStretch, bordered, allowArbRatio;
    UInt32      rotateFlags;

    if( width < 640.0)
        return( 1 );
    if( height < 480.0)
        return( 2 );

    if( width > connectRef->scalerInfo->maxHorizontalPixels)
        return( 3 );
    if( height > connectRef->scalerInfo->maxVerticalPixels)
        return( 4 );

    if( width < nativeWidth)
        need |= kIOScaleCanUpSamplePixels;
    else if( width != nativeWidth)
        need |= kIOScaleCanDownSamplePixels;
    if( height < nativeHeight)
        need |= kIOScaleCanUpSamplePixels;
    else if( height != nativeHeight)
        need |= kIOScaleCanDownSamplePixels;

    rotateFlags = kIOScaleRotateFlags & connectRef->transform;
    if (rotateFlags)
        need |= kIOScaleCanRotate;

    if( need != (need & connectRef->scalerInfo->scalerFeatures))
        return( 5 );

    aspectDiff = ratioOver( nativeWidth / nativeHeight, width / height );

    bordered = ((width == nativeWidth) || (height == nativeHeight));
    allowArbRatio = (0 != ((kIOScaleCanScaleInterlaced | kIOScaleCanRotate) & connectRef->scalerInfo->scalerFeatures));

    okToStretch = ((0 == (kScaleInstallNoStretch & flags)) && (aspectDiff > 1.03125) && (aspectDiff < 1.5));

    if (0 == (kScaleInstallAlways & flags))
    {
        ratio = (width / nativeWidth);
        if( (ratio < 1.18) && (ratio > 0.82))
        {
            if (bordered || allowArbRatio)
                okToStretch = false;
            else
                return( 6 );
        }
        ratio = (height / nativeHeight);
        if( (ratio < 1.18) && (ratio > 0.82))
        {
            if (bordered || allowArbRatio)
                okToStretch = false;
            else
                return( 7 );
        }
        if( aspectDiff > 2.0)
            return( 8 );
    }

    *desc = *baseDesc;

    desc->timingInfo.detailedInfo.v2.horizontalScaled = ((UInt32) ceilf(width));
    desc->timingInfo.detailedInfo.v2.verticalScaled   = ~1 & ((UInt32) ceilf(height));
    desc->timingInfo.detailedInfo.v2.scalerFlags = 0;

    UpdateTimingInfoForTransform(connectRef, desc, flags);

    desc->info.flags = (desc->info.flags & ~(kDisplayModeSafetyFlags | kDisplayModeNativeFlag))
                        | kDisplayModeValidFlag | kDisplayModeSafeFlag;

    if( aspectDiff > 1.03125)
        desc->info.flags |= kDisplayModeNotPresetFlag;

	desc->info.flags |= setModeFlags;
	desc->info.flags &= ~clrModeFlags;
	
    if( 0 == (kIOScaleStretchOnly & connectRef->scalerInfo->scalerFeatures))
    {
        IOFBInstallScaledMode( connectRef, desc, flags );
    }

    if (false && okToStretch)
    {
        desc->info.flags |= kDisplayModeStretchedFlag;
		desc->info.flags &= ~clrModeFlags;
        desc->timingInfo.detailedInfo.v2.scalerFlags |= kIOScaleStretchToFit;
        IOFBInstallScaledMode( connectRef, desc, flags );
    }

    return( 0 );
}

static kern_return_t
IOFBInstallScaledResolution( IOFBConnectRef connectRef,
                       IOFBDisplayModeDescription * desc,
                       float nativeWidth, float nativeHeight,
                       float width, float height,
                       IOOptionBits flags,
					   uint32_t setModeFlags, uint32_t clrModeFlags )
{
    int diag1, diag2;

    diag1 = _IOFBInstallScaledResolution(connectRef, desc, nativeWidth, nativeHeight, width, height,
    												flags, setModeFlags, clrModeFlags);
    DEBG(connectRef, "(%d) %f x %f, %08x\n", diag1, width, height, (int) flags);

    if ((kIOFBSwapAxes | kIOScaleSwapAxes) & connectRef->transform)
    {
        if (ratioOver(width / height, 4.0 / 3.0) <= 1.03125)
        {
            flags |= kScaleInstallNoResTransform;
            diag2 = _IOFBInstallScaledResolution(connectRef, desc, nativeWidth, nativeHeight, width, height,
            										flags, setModeFlags, clrModeFlags);
            DEBG(connectRef, "(%d) %f x %f, %08x\n", diag2, width, height, (int) flags );
        }
    }

    return (diag1 ? kIOReturnUnsupported : kIOReturnSuccess);
}

static Boolean
IOFBLookScaleBaseMode( IOFBConnectRef connectRef, IOFBDisplayModeDescription * scaleBase,
                        IOFBDisplayModeDescription * scaleDesc )
{
    Boolean found = false;
    UInt32 h, v;

    DEBG(connectRef, "%d: %dx%d %fHz scale %dx%d %08x %08x\n",
           (int) scaleBase->timingInfo.appleTimingID,
           (int) scaleBase->timingInfo.detailedInfo.v2.horizontalActive,
           (int) scaleBase->timingInfo.detailedInfo.v2.verticalActive,
           RefreshRateFromDetailedTiming(&scaleBase->timingInfo.detailedInfo.v2),
           (int) scaleBase->timingInfo.detailedInfo.v2.horizontalScaled,
           (int) scaleBase->timingInfo.detailedInfo.v2.verticalScaled,
           (int) scaleBase->info.flags, (int) scaleBase->timingInfo.flags);

    do {
        if( 0 == (kIODetailedTimingValid & scaleBase->timingInfo.flags))
            continue;

        if( (kDisplayModeValidFlag | kDisplayModeSafeFlag) !=
            ((kDisplayModeValidFlag | kDisplayModeSafeFlag) & scaleBase->info.flags))
            continue;

        if( (kDisplayModeBuiltInFlag
            | kDisplayModeNeverShowFlag
            | kDisplayModeStretchedFlag
            | kDisplayModeNotGraphicsQualityFlag
            | kDisplayModeNotPresetFlag) & scaleBase->info.flags)
            continue;

        if ((kDisplayModeInterlacedFlag & scaleBase->info.flags)
         && (!(kIOScaleCanScaleInterlaced & connectRef->scalerInfo->scalerFeatures)))
            continue;

        if ((kDisplayModeNativeFlag & scaleDesc->info.flags)
            && !(kDisplayModeNativeFlag & scaleBase->info.flags))
            continue;
        
        if ((kDisplayModeNativeFlag & scaleBase->info.flags)
            && !(kDisplayModeNativeFlag & scaleDesc->info.flags))
        {
            found = true;
            continue;
        }
        
        if (!(kDisplayModeNativeFlag & scaleDesc->info.flags) && (kDisplayModeDefaultFlag & scaleDesc->info.flags)
            && !(kDisplayModeDefaultFlag & scaleBase->info.flags))
            continue;

#if 0
        if(connectRef->driverModeCount
         && (kIOTimingIDApple_FixedRateLCD != scaleBase->timingInfo.appleTimingID))
            continue;
#endif

        if (kIOScaleSwapAxes & connectRef->transform)
        {
            h = scaleBase->timingInfo.detailedInfo.v2.verticalScaled;
            v = scaleBase->timingInfo.detailedInfo.v2.horizontalScaled;
        }
        else
        {
            h = scaleBase->timingInfo.detailedInfo.v2.horizontalScaled;
            v = scaleBase->timingInfo.detailedInfo.v2.verticalScaled;
        }
        if (h && (h != scaleBase->timingInfo.detailedInfo.v2.horizontalActive))
            continue;
        if (v && (v != scaleBase->timingInfo.detailedInfo.v2.verticalActive))
            continue;

        if( scaleBase->timingInfo.detailedInfo.v2.horizontalActive
            < scaleDesc->timingInfo.detailedInfo.v2.horizontalActive)
            continue;
        if( scaleBase->timingInfo.detailedInfo.v2.verticalActive
            < scaleDesc->timingInfo.detailedInfo.v2.verticalActive)
            continue;

        if((scaleBase->timingInfo.detailedInfo.v2.horizontalActive
            == scaleDesc->timingInfo.detailedInfo.v2.horizontalActive)
        && (scaleBase->timingInfo.detailedInfo.v2.verticalActive
            == scaleDesc->timingInfo.detailedInfo.v2.verticalActive)
        && (RefreshRateFromDetailedTiming(&scaleBase->timingInfo.detailedInfo.v2)
            < RefreshRateFromDetailedTiming(&scaleDesc->timingInfo.detailedInfo.v2)))
            continue;

        if ((kDisplayModeInterlacedFlag & scaleBase->info.flags)
            && (!(kDisplayModeInterlacedFlag & scaleDesc->info.flags)))
            continue;

        found = true;

    } while( false );
    
    if (found)
    {
        DEBG(connectRef, "choosing\n");
        
        *scaleDesc = *scaleBase;
        scaleDesc->timingInfo.appleTimingID = 0;
        scaleDesc->timingInfo.flags = kIODetailedTimingValid;
    }

    return( found );
}

static kern_return_t
IOFBInstallScaledModes( IOFBConnectRef connectRef, IOFBDisplayModeDescription * scaleBase,
						Boolean onlyMirrorModes )
{
    IOReturn                    err = kIOReturnSuccess;
	IOFBConnectRef              next;
    CFMutableArrayRef           arrays;
    CFArrayRef                  iogArray = NULL;
    CFArrayRef                  displayArray = NULL;
    CFArrayRef                  otherArray = NULL;
    CFIndex                     count, arraysCount;
    SInt32                      i, arraysIdx;
    float                       h, v, nh, nv;
    Boolean                     displayNot4By3, displayNot16By9, display4k, other4k;

    if( !connectRef->scalerInfo) return( kIOReturnSuccess );
    if( kOvrFlagDisableScaling & connectRef->ovrFlags) return( kIOReturnSuccess );

	arrays = CFArrayCreateMutable( kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks );
    if (!arrays) return( kIOReturnNoMemory );

    nh = (float) scaleBase->timingInfo.detailedInfo.v2.horizontalActive;
    nv = (float) scaleBase->timingInfo.detailedInfo.v2.verticalActive;
    
    DEBG(connectRef, "Scaling mode (%f,%f)\n", nh, nv);

    display4k = ((nh >= 3840) || (nv >= 3840)); // Include rotated 4k displays. Consider kIOMinPixelsWithDownscaleModes
    
    if( display4k ) {
        iogArray = CFDictionaryGetValue( gIOGraphicsProperties, CFSTR("scale-resolutions-4k") );
    } else {
        iogArray = CFDictionaryGetValue( gIOGraphicsProperties, CFSTR("scale-resolutions") );
    }
    
	next = connectRef;
	do {
		if (next->overrides) {
            other4k = ((next->defaultWidth >= 3840) || (next->defaultHeight >= 3840)) && (next != connectRef);
            
            otherArray = CFDictionaryGetValue( next->overrides, CFSTR("scale-resolutions") );
            
            if(other4k && (otherArray == NULL)) {
                otherArray = CFDictionaryGetValue( gIOGraphicsProperties, CFSTR("scale-resolutions-4k") );
            }
            
            if (otherArray) {
                count = CFArrayGetCount(otherArray);
                
                CFMutableArrayRef arrayWithHeight = CFArrayCreateMutable(kCFAllocatorDefault, count, &kCFTypeArrayCallBacks);
                
                for( i = 0; i < count; i++) {
                    CFTypeRef obj;
                    IOOptionBits flags;
                    uint32_t setModeFlags = 0;
                    uint32_t clrModeFlags = 0;
                    
                    obj = CFArrayGetValueAtIndex(otherArray, i);
                    if( CFDataGetTypeID() == CFGetTypeID(obj)) {
                        CFIndex length = CFDataGetLength((CFDataRef) obj);
                        
                        UInt32 updatedValue[5];
                        UInt32 * value = (UInt32 *) CFDataGetBytePtr((CFDataRef) obj);
                        h     = (float) OSReadBigInt32(&value[0], 0);
                        v     = (float) OSReadBigInt32(&value[1], 0);
                        flags =         OSReadBigInt32(&value[2], 0);
                        updatedValue[0] = value[0];
                        updatedValue[1] = value[1];
                        updatedValue[2] = value[2];
                        if (length >= (CFIndex)(4 * sizeof(*value))) {
                            setModeFlags = OSReadBigInt32(&value[3], 0);
                            updatedValue[3] = value[3];
                        }
                        if (length >= (CFIndex)(5 * sizeof(*value))) {
                            clrModeFlags = OSReadBigInt32(&value[4], 0);
                            updatedValue[4] = value[4];
                        }
                        
                        if (v == 0) {
                            v = h / next->nativeAspect;
                            UInt32 intV = (UInt32) v;
                            OSWriteBigInt32(&updatedValue[1], 0, intV);
                        }
                        
                        CFDataRef updatedData = CFDataCreate(kCFAllocatorDefault, (UInt8 *)updatedValue, length);
                        CFArrayAppendValue(arrayWithHeight, updatedData);
                        CFRelease(updatedData);
                        
                    } else
                        CFArrayAppendValue(arrayWithHeight, obj);
                }

                CFArrayAppendValue( arrays, arrayWithHeight );
                CFRelease(arrayWithHeight);
                otherArray = arrayWithHeight;
            }
            
			if (next == connectRef) 
			{
			    displayArray = otherArray;
				if (iogArray) CFArrayAppendValue(arrays, iogArray);
			}
		}
		next = next->nextDependent;
	} while( next && (next != connectRef) );

    if (!onlyMirrorModes
     && ((nh <= (2 * kAquaMinWidth)) || (nv >= (2 * kAquaMinHeight))))
        IOFBInstallScaledResolution( connectRef, scaleBase, nh, nv, nh / 2.0, nv / 2.0, 0, 0, 0 );

    displayNot4By3  = (ratioOver(nh / nv, 4.0 / 3.0) > 1.03125);
    displayNot16By9 = (ratioOver(nh / nv, 16.0 / 9.0) > 1.03125);

    arraysCount = CFArrayGetCount(arrays);
    for(arraysIdx = 0; arraysIdx < arraysCount; arraysIdx++)
    {
    	CFArrayRef array = CFArrayGetValueAtIndex(arrays, arraysIdx);
		count = CFArrayGetCount(array);
		for( i = 0; i < count; i++) {
			CFTypeRef obj;
			IOReturn r;
			IOOptionBits flags;
			uint32_t setModeFlags = 0;
			uint32_t clrModeFlags = 0;
	
			obj = CFArrayGetValueAtIndex(array, i);
			if( CFNumberGetTypeID() == CFGetTypeID(obj)) {
				SInt32      value;
				CFNumberGetValue( (CFNumberRef) obj, kCFNumberSInt32Type, &value );
				h     = (float)(value & 0xffff);
				v     = (float)(value >> 16);
	
				flags = (array == displayArray) ? (kScaleInstallAlways | kScaleInstallNoStretch) : 0;
	
			} else if( CFDataGetTypeID() == CFGetTypeID(obj)) {
				UInt32 * value = (UInt32 *) CFDataGetBytePtr((CFDataRef) obj);
				h     = (float) OSReadBigInt32(&value[0], 0);
				v     = (float) OSReadBigInt32(&value[1], 0);
				flags =         OSReadBigInt32(&value[2], 0);
				if (CFDataGetLength((CFDataRef) obj) >= (CFIndex)(4 * sizeof(*value))) {
					setModeFlags = OSReadBigInt32(&value[3], 0);
				}
				if (CFDataGetLength((CFDataRef) obj) >= (CFIndex)(5 * sizeof(*value))) {
					clrModeFlags = OSReadBigInt32(&value[4], 0);
				}
	
			} else
				continue;

			DEBG(connectRef, "Scaling to (%f,%f), 0x%x, |0x%x, &0x%x\n", h, v, (int) flags, setModeFlags, clrModeFlags);

			bool downscaleOk = ((array != iogArray)
						      || (kMirrorOnlyFlags & setModeFlags)
                              || display4k );

			if ((kOvrFlagDisableGenerated & connectRef->ovrFlags) && (array == iogArray)) {
				if ((0 == (kMirrorOnlyFlags & setModeFlags))) continue;
			}

			if ((array == iogArray) || (array == displayArray)) {
				if (onlyMirrorModes && (0 == (kMirrorOnlyFlags & setModeFlags))) continue;
			} else {
				if (!(kScaleInstallMirrorDeps & flags)) continue;
			}

			// downsampled modes from override only
			if ((!downscaleOk) && (h > nh)) continue;
		   
			if( v) {
				if ((!downscaleOk) && (v > nv)) continue;
				if( (h != (nh / 2.0)) || (v != (nv / 2.0))) {
					r = IOFBInstallScaledResolution( connectRef, scaleBase, 
														nh, nv, h, v, flags, setModeFlags, clrModeFlags );
				}
			} else {
				if( displayNot4By3 && (h < 1920) && (v < 1920)) {          // Add legacy 4:3 modes but not for larger sizes
					v = (h * 3.0) / 4.0;
					if ((downscaleOk) || (v <= nv)) {
						r = IOFBInstallScaledResolution( connectRef, scaleBase,
															nh, nv, h, v, flags, setModeFlags, clrModeFlags );
					}
				}
				if((h != nh) && (h != (nh / 2.0))) {
					v = (h * nv) / nh;
					if ((downscaleOk) || (v <= nv)) {
						r = IOFBInstallScaledResolution( connectRef, scaleBase,
														nh, nv, h, v, flags, setModeFlags, clrModeFlags );
					}
				}
			}
		}
	}

	if( displayNot16By9) {
		h = nh;
		v = (h * 9.0) / 16.0;
		IOFBInstallScaledResolution( connectRef, scaleBase,
												nh, nv, h, v, kScaleInstallAlways, kDisplayModeValidForAirPlayFlag, 0 );
	}

    CFRelease( arrays );

    return( err );
}


__private_extern__ Boolean
IOFBTimingSanity(IOTimingInformation * timingInfo)
{
    if (true
     && (timingInfo->detailedInfo.v2.horizontalScaled == timingInfo->detailedInfo.v2.horizontalActive)
     && (timingInfo->detailedInfo.v2.verticalScaled   == timingInfo->detailedInfo.v2.verticalActive)
     && (!timingInfo->detailedInfo.v2.horizontalScaledInset)
     && (!timingInfo->detailedInfo.v2.verticalScaledInset)
     && (!(kIOScaleRotateFlags & timingInfo->detailedInfo.v2.scalerFlags)))
    {
        timingInfo->detailedInfo.v2.horizontalScaled = 0;
        timingInfo->detailedInfo.v2.verticalScaled   = 0;
        timingInfo->detailedInfo.v2.scalerFlags      = 0;
    }

    return (true);
}

__private_extern__ kern_return_t
IOFBDriverPreflight(IOFBConnectRef connectRef, IOFBDisplayModeDescription * desc)
{
    kern_return_t result;
    IOFBDisplayModeDescription descOut;

    if (kIOFBConnectStateUnusable & connectRef->state)
    {
        return (kIOReturnOffline);
    }

    size_t len = sizeof(IOFBDisplayModeDescription);
    result = IOConnectCallStructMethod(connectRef->connect, 17, // Index
                                    desc, len, &descOut, &len);

    if ((kIOReturnSuccess != result)
        || !IOFBTimingSanity(&desc->timingInfo)
        || !ValidateTimingInformation(connectRef, &desc->timingInfo))
    {
#if RLOG
        DEBG(connectRef, "preflight fail (%x)\n", result);
        IOFBLogTiming(connectRef, &desc->timingInfo);
#endif
        result = kIOReturnUnsupportedMode;
    }

    DEBG(connectRef, "preflight (%x) %d x %d %f Hz\n", 
                result, (int) descOut.info.nominalWidth, (int) descOut.info.nominalHeight,
                descOut.info.refreshRate / 65536.0);
    
    if (kIOReturnSuccess == result)
    {
        desc->info.nominalWidth  = descOut.info.nominalWidth;
        desc->info.nominalHeight = descOut.info.nominalHeight;
//      desc->info.refreshRate   = descOut.info.refreshRate;
        desc->info.maxDepthIndex = descOut.info.maxDepthIndex;
        desc->info.flags         = desc->info.flags
        			 | (kDisplayModeAcceleratorBackedFlag & descOut.info.flags);
//      desc->info.reserved      = descOut.info.reserved;
    }

    return (result);
}

static kern_return_t
IOFBCreateDisplayModeInformation(
        IOFBConnectRef                  connectRef,
        IODisplayModeID                 displayMode,
        IOFBDisplayModeDescription *    allInfo )
{
    kern_return_t               kr;

    if (kIOFBSWOfflineDisplayModeID == displayMode)
    {
        allInfo->info.nominalWidth = 1;
        allInfo->info.nominalHeight = 1;
        allInfo->info.refreshRate = 0;
        allInfo->info.maxDepthIndex = 0;
        allInfo->info.flags = kDisplayModeValidFlag 
                        | kDisplayModeSafeFlag 
                        | kDisplayModeDefaultFlag;
        allInfo->timingInfo.flags = 0;
        allInfo->timingInfo.appleTimingID = kIOTimingIDApple_0x0_0hz_Offline;

        return (kIOReturnSuccess);
    }

    uint64_t inData = displayMode;
    size_t len = sizeof(IOFBDisplayModeDescription);
    kr = IOConnectCallMethod(connectRef->connect, 5,    // index
            &inData, 1, NULL,    0,                     // Input
            NULL, NULL, allInfo, &len);                 // Output

    if (len < sizeof(IOFBDisplayModeDescription))
        kr = kIOReturnUnderrun;

    if (kIOReturnSuccess == kr)
        IOFBTimingSanity(&allInfo->timingInfo);

    return( kr );
}

static kern_return_t
IOFBAdjustDisplayModeInformation(
        IOFBConnectRef                  connectRef,
        IODisplayModeID                 displayMode,
        IOFBDisplayModeDescription *    allInfo )
{
    IOReturn                    result;
    CFDataRef                   edidData;
    EDID *                      edid = 0;
    CFDictionaryRef             ovr = 0;
    IOAppleTimingID             appleTimingID;
    UInt8                       manufacturerFlag;
    bool                        addSafeFlag;

    appleTimingID = allInfo->timingInfo.appleTimingID;

    DEBG(connectRef, "%d x %d @ %d (%x,%d): %08x %08x\n", 
        (int) allInfo->info.nominalWidth, (int) allInfo->info.nominalHeight,
        (int) (allInfo->info.refreshRate + 0x8000) >> 16, (int) displayMode, (int) appleTimingID,
        (int) allInfo->info.flags, (int) allInfo->timingInfo.flags);

    switch( appleTimingID ) {
        case kIOTimingIDAppleNTSC_ST:
        case kIOTimingIDAppleNTSC_FF:
        case kIOTimingIDAppleNTSC_STconv:
        case kIOTimingIDAppleNTSC_FFconv:
            allInfo->info.flags |= kDisplayModeTelevisionFlag;
            manufacturerFlag = kAppleNTSCManufacturerFlag | kAppleNTSCDefaultPALManufacturerFlag;
            break;

        case kIOTimingIDApplePAL_ST:
        case kIOTimingIDApplePAL_FF:
        case kIOTimingIDApplePAL_STconv:
        case kIOTimingIDApplePAL_FFconv:
            allInfo->info.flags |= kDisplayModeTelevisionFlag;
            manufacturerFlag = kApplePALManufacturerFlag;
            break;

        default:
            manufacturerFlag = 0x00;
            break;
    }

    if (connectRef->addTVFlag)
        allInfo->info.flags |= kDisplayModeTelevisionFlag;

    do {
        ovr = connectRef->overrides;
        if( !ovr)
            continue;

        edidData = CFDictionaryGetValue(ovr, CFSTR(kIODisplayEDIDKey));
        if( edidData)
            edid = (EDID *) CFDataGetBytePtr(edidData);

        addSafeFlag = ((kAddSafeFlags == (kAddSafeFlags & allInfo->info.flags)) && !connectRef->hasCEAExt);
    
        if((kDisplayModeBuiltInFlag & allInfo->info.flags) && !addSafeFlag)
            continue;
    
        if (0 == (kDisplayModeNeverShowFlag & allInfo->info.flags))
        {
            if (GetTovr(connectRef, appleTimingID, &allInfo->info.flags, NULL))
                continue;
        }
    
        if( kOvrFlagDisableNonScaled & connectRef->ovrFlags) {
            if( (displayMode > 0) && (0 == (kDisplayModeDefaultFlag & allInfo->info.flags)))
                allInfo->info.flags &= ~kDisplayModeSafetyFlags;
        }
    
#if 1
        if((kDisplayModeValidFlag & allInfo->info.flags) && !addSafeFlag)
            continue;
#endif
        if( displayMode < 0)                                    // programmed mode
            continue;

        // 2488698, 3052614
        if( appleTimingID == kIOTimingIDApple_FixedRateLCD
            /*&& !CFDictionaryGetValue( ovr, CFSTR(kIODisplayIsDigitalKey))*/)
            continue;

        if ((allInfo->timingInfo.detailedInfo.v2.scalerFlags) && !addSafeFlag)
            continue;

        if( appleTimingID == kIOTimingIDApple_0x0_0hz_Offline)
            continue;
#if 1
        if( kDisplayModeNeverShowFlag & allInfo->info.flags)
            continue;
#endif

        if( (kDisplayAppleVendorID == connectRef->displayVendor)
            && edid && edid->version
            && ((edid->version > 1) || (edid->revision >= 3))) {
    
            if( manufacturerFlag & edid->establishedTimings[2]) {
                allInfo->info.flags |= kDisplayModeValidFlag | kDisplayModeSafeFlag;

                if ((kApplePALManufacturerFlag == manufacturerFlag)
                 && (kApplePALManufacturerFlag == ((kAppleNTSCManufacturerFlag | kApplePALManufacturerFlag) 
                                                    & edid->establishedTimings[2])))
                    allInfo->info.flags |= kDisplayModeDefaultFlag;
                continue;
            }
        }
    
        if((kDisplayModeInterlacedFlag & allInfo->info.flags) && !addSafeFlag)
            continue;

        result = IOCheckTimingWithDisplay(connectRef, allInfo, kIOFBDriverMode);
        if (kIOReturnNotFound == result)
            continue;
        allInfo->info.flags &= ~kDisplayModeSafetyFlags;
        if (kIOReturnSuccess != result)
            continue;

        allInfo->info.flags |= kDisplayModeValidFlag | kDisplayModeSafeFlag;

        if( (allInfo->timingInfo.detailedInfo.v2.horizontalActive > connectRef->dimensions.width)
            || (allInfo->timingInfo.detailedInfo.v2.verticalActive > connectRef->dimensions.height)) {
            allInfo->info.flags |= connectRef->dimensions.setFlags;
            allInfo->info.flags &= ~connectRef->dimensions.clearFlags;
        }

    } while( false );

    if ((kDisplayModeValidFlag & allInfo->info.flags) 
    	&& !edid 
    	&& (kIOFBConnectStateOnline & connectRef->state))
    {
    	allInfo->info.flags |= kDisplayModeAlwaysShowFlag;
    }

    return( kIOReturnSuccess );
}

kern_return_t
_IOFBGetDisplayModeInformation(IOFBConnectRef connectRef,
        IODisplayModeID         displayMode,
        IODisplayModeInformation * out )
{
    kern_return_t              kr = kIOReturnSuccess;
    CFDataRef                  data;
    CFMutableDataRef           piData;
    CFMutableDictionaryRef     dict;
    IODisplayModeInformation * info;

    dict = (CFMutableDictionaryRef) CFDictionaryGetValue( connectRef->modes,
                                (const void *) (uintptr_t) (UInt32) displayMode );
    if( dict && (data = CFDictionaryGetValue( dict, CFSTR(kIOFBModeDMKey) )))
        info = (IODisplayModeInformation *) CFDataGetBytePtr(data);
    else
    {
        DEBG(connectRef, "invalid mode 0x%x\n", (int) displayMode);
        kr = kIOReturnBadArgument;
    }

    if( kr == kIOReturnSuccess)
    {
        *out = *info;
        if( (displayMode == connectRef->defaultMode) && (out->flags & kDisplayModeValidFlag))
            out->flags |= kDisplayModeDefaultFlag;
        else
            out->flags &= ~kDisplayModeDefaultFlag;

//        if (kMirrorOnlyFlags & out->flags)
//            out->flags &= ~kDisplayModeValidFlag;

        if(true && connectRef->suppressRefresh)
            out->refreshRate = 0;
        else if(connectRef->detailedRefresh)
        {
            // /panther prefs workaround
            out->refreshRate += 0x00000800;
            out->refreshRate &= 0xffffe000;
            out->refreshRate |= 1;
            // panther prefs workaround/
        }
        else
        {
            out->refreshRate += 0x00008000;
            out->refreshRate &= 0xffff0000;
        }
        if (kIOFBSwapAxes & connectRef->transform)
        {
            SInt32 width = out->nominalWidth;
            out->nominalWidth = out->nominalHeight;
            out->nominalHeight = width;
        }

        piData = (CFMutableDataRef) CFDictionaryGetValue(dict, CFSTR(kIOFBModePIKey));
        if (!piData && (piData = CFDataCreateMutable(kCFAllocatorDefault, 0)))
        {
            IOReturn           err;
            IOPixelInformation pixelInfo;
            IOIndex            depth;

            for (depth = 0; depth <= out->maxDepthIndex; depth++)
            {
                err = _IOFBGetPixelInformation(connectRef, displayMode, depth,
                                               kIOFBSystemAperture, &pixelInfo);
                if (kIOReturnSuccess != err)
                    break;
                if (pixelInfo.bitsPerPixel > FILTER_MAXDEPTH)
                    break;
                CFDataAppendBytes(piData, (UInt8 *) &pixelInfo, sizeof(IOPixelInformation));
            }
            CFDictionarySetValue(dict, CFSTR(kIOFBModePIKey), piData);
            CFRelease(piData);
        }
        if (piData)
        {
            out->maxDepthIndex = CFDataGetLength(piData) / sizeof(IOPixelInformation);
            if (out->maxDepthIndex)
                out->maxDepthIndex--;
        }
    }

    return( kr );
}
kern_return_t
IOFBGetDisplayModeInformation( io_connect_t connect,
        IODisplayModeID         displayMode,
        IODisplayModeInformation * out )
{
    IOFBConnectRef connectRef;

    connectRef = IOFBConnectToRef( connect);
    if( !connectRef)
        return( kIOReturnBadArgument );

	return (_IOFBGetDisplayModeInformation(connectRef, displayMode, out));
}


kern_return_t
IOFBGetDisplayModeTimingInformation( io_connect_t connect,
        IODisplayModeID               displayMode,
        IODetailedTimingInformation * out )
{
    kern_return_t                 kr = kIOReturnSuccess;
    IOFBConnectRef                connectRef;
    CFDataRef                     data;
    CFMutableDictionaryRef        dict;
    IODetailedTimingInformation * info;

    connectRef = IOFBConnectToRef( connect);
    if( !connectRef)
        return( kIOReturnBadArgument );

    dict = (CFMutableDictionaryRef) CFDictionaryGetValue( connectRef->modes,
                                (const void *) (uintptr_t) (UInt32) displayMode );
    if( dict && (data = CFDictionaryGetValue( dict, CFSTR(kIOFBModeTMKey) )))
        info = (IODetailedTimingInformation *) CFDataGetBytePtr(data);
    else
    {
        DEBG(connectRef, "invalid mode 0x%x\n", (int) displayMode);
        kr = kIOReturnBadArgument;
    }

    if( kr == kIOReturnSuccess)
    {
        *out = *info;
	}

	return (kr);
}

__private_extern__
IOFBConnectRef IOFBConnectToRef( io_connect_t connect )
{
    return((IOFBConnectRef) CFDictionaryGetValue( gConnectRefDict, (void *) (uintptr_t) connect ));
}

static kern_return_t
IOFramebufferServerOpen( mach_port_t connect )
{
    mach_port_t                 masterPort;
    IOFBConnectRef              connectRef, next;
    IOReturn                    err;
    CFNumberRef                 num;

    if (gConnectRefDict && IOFBConnectToRef(connect))
        return (kIOReturnSuccess);

    do {

        err = kIOReturnNoMemory;

        IOMasterPort( MACH_PORT_NULL, &masterPort );

        if( !gConnectRefDict)
            gConnectRefDict = CFDictionaryCreateMutable(
                kCFAllocatorDefault, (CFIndex) 0,
                (CFDictionaryKeyCallBacks *) 0,
                (CFDictionaryValueCallBacks *) 0 );     //&kCFTypeDictionaryValueCallBacks
        if( !gConnectRefDict)
            return( kIOReturnNoMemory );
    
        connectRef = calloc( 1, sizeof( struct IOFBConnect));
        if( !connectRef)
            continue;
    
        connectRef->connect = connect;
        err = IOConnectGetService( connect, &connectRef->framebuffer );
        if( kIOReturnSuccess != err)
            continue;

        connectRef->iographicsProperties = gIOGraphicsProperties;
        connectRef->defaultMinWidth  = gIOGraphicsInstallBoot ? kInstallMinWidth : kAquaMinWidth;
        connectRef->defaultMinHeight = gIOGraphicsInstallBoot ? kInstallMinHeight : kAquaMinHeight;
        connectRef->inMuxSwitch = false;

#if RLOG
        if (gAllConnects)
        {
            connectRef->logfile = gAllConnects->logfile;
            connectRef->time0   = gAllConnects->time0;
        }
        else
        {
            connectRef->logfile = fopen(kIOGraphicsLogfilePath, "w" /*"r+"*/);
            connectRef->time0   = mach_absolute_time();
        }
        DEBG(connectRef, "\n" );
#endif
    
        CFDictionarySetValue( gConnectRefDict, (const void *) (uintptr_t) connect, connectRef );

        num = IORegistryEntryCreateCFProperty( connectRef->framebuffer, CFSTR(kIOFBDependentIDKey),
                                                kCFAllocatorDefault, kNilOptions );
        if( num) { 
            CFNumberGetValue( num, kCFNumberSInt64Type, &connectRef->dependentID );
            CFRelease(num);
        }
        num = IORegistryEntryCreateCFProperty( connectRef->framebuffer, CFSTR(kIOFBDependentIndexKey),
                                                kCFAllocatorDefault, kNilOptions );
        if( num) { 
            CFNumberGetValue( num, kCFNumberSInt32Type, &connectRef->dependentIndex );
            CFRelease(num);
        } else
            connectRef->dependentID = 0;

        // add to dependent list
        if( connectRef->dependentID) {
            for( next = gAllConnects; next; next = next->next) {
                if( next->dependentID == connectRef->dependentID) {
    
                    if( next->nextDependent)
                        connectRef->nextDependent = next->nextDependent;
                    else
                        connectRef->nextDependent = next;
                    next->nextDependent = connectRef;
                    break;
                }
            }
        }
        // add to all list
        connectRef->next = gAllConnects;
        gAllConnects = connectRef;
        // --
    
        connectRef->notifyPort = IONotificationPortCreate( masterPort );
        if( !connectRef->notifyPort)
            return( kIOReturnError );

        IOConnectSetNotificationPort( connect, 0,
                        IONotificationPortGetMachPort( connectRef->notifyPort ), 0);

        err = IOServiceAddInterestNotification(
                connectRef->notifyPort,
                connectRef->framebuffer,
                kIOGeneralInterest,
                &IOFBInterestCallback, connectRef,
                &connectRef->interestNotifier );

    } while( false );

    return (kIOReturnSuccess);
}

kern_return_t
IOFramebufferServerFinishOpen( io_connect_t connect )
{
    IOFBConnectRef next, connectRef;
    SInt32         pass, dependentIndex;

	connectRef = IOFBConnectToRef(connect);
	if (!connectRef) return(kIOReturnBadArgument);

    if (connectRef->opened) return (kIOReturnSuccess);

    DEBG(connectRef, "IOFramebufferServerFinishOpen start\n");

    uint32_t onlineCount = 0;
    for (pass = 0; pass < 3; pass++)
    {
		for (dependentIndex = 0; dependentIndex < 32; dependentIndex++)
		{
			next = connectRef;
			do
			{
				if (next->dependentIndex == dependentIndex)
				{
					if (pass == 0)
					{
						next->opened = true;
						next->setKernelDisplayConfig = true;
						IOFBUpdateConnectState(next);
						if (kIOFBConnectStateOnline & next->state) onlineCount++;
					}
					if (pass <= 1) 
					{
						IOFBRebuild(next, false);
						if (kIOFBConnectStateUnusable & next->state)
						{
							next->state &= ~kIOFBConnectStateOnline;
						}
					}

					if (pass == 2) IOFramebufferFinishOpen(next);
				}
				next = next->nextDependent;
			}
			while (next && (next != connectRef));
		}
		if ((pass == 0) && (onlineCount <= 1)) pass++;
	}

	DEBG(connectRef, "IOFramebufferServerFinishOpen end\n");
    
	return (kIOReturnSuccess);
}

static kern_return_t
IOFramebufferFinishOpen(IOFBConnectRef connectRef)
{
    IODisplayModeID             mode, otherMode;
    IOIndex                     depth, minDepth, otherDepth;
    IODisplayModeID             startMode;
    IOIndex                     startDepth;
    UInt32                      startFlags = 0;
    IOReturn                    err;
    IODisplayModeInformation *  otherInfo, info;
    CFDictionaryRef             dict;
    CFDataRef                   data;

    if (!(kIOFBConnectStateOnline & connectRef->state)) return (kIOReturnSuccess);

    do
    {
        err = _IOFBGetCurrentDisplayModeAndDepth( connectRef, &mode, &depth );
        startMode = mode;
        startDepth = depth;
        if( err)
            continue;

        err = _IOFBGetDisplayModeInformation( connectRef, startMode, &info);
        if( err)
            continue;

        startFlags = info.flags;
        if( (info.nominalWidth  < connectRef->defaultMinWidth)
          || (info.nominalHeight < connectRef->defaultMinHeight)) {
            err = kIOReturnNoResources;
            continue;
        }

        if( !connectRef->relaunch) {
            if( connectRef->make4By3 && connectRef->default4By3Mode
                && ratioOver(((float)info.nominalWidth) / ((float)info.nominalHeight), 4.0 / 3.0) > 1.03125) {
                err = kIOReturnNoResources;
                continue;
            }
    
            if( connectRef->defaultToDependent
            && (kIOReturnSuccess == _IOFBGetCurrentDisplayModeAndDepth( connectRef->nextDependent,
                                                                            &otherMode, &otherDepth ))
            && (dict = CFDictionaryGetValue( connectRef->nextDependent->modes, (const void *) (uintptr_t) (UInt32) otherMode ))
            && (data = CFDictionaryGetValue( dict, CFSTR(kIOFBModeDMKey) ))) {
    
                otherInfo = (IODisplayModeInformation *) CFDataGetBytePtr(data);
                if( (otherInfo->nominalWidth  != info.nominalWidth)
                   || (otherInfo->nominalHeight != info.nominalHeight)) {
                    err = kIOReturnNoResources;
                    continue;
                }
                startDepth = otherDepth;
            }
        }

        // make sure it does >= 16bpp
        minDepth = IOFBIndexForPixelBits( connectRef, startMode, info.maxDepthIndex, 16 );
        if( minDepth < 0) {
            err = kIOReturnNoResources;
            continue;
        }

        if( connectRef->firstBoot) {
            // default depth on first boot
            startDepth = minDepth;
            if( info.maxDepthIndex > minDepth)
                startDepth++;
        } else if( startDepth < minDepth)
            startDepth = minDepth;

    } while( false );

    if( err
        || (connectRef->firstBoot && (kDisplayVendorIDUnknown  != connectRef->displayVendor)
                      && (kDisplayProductIDGeneric != connectRef->displayProduct))
        || (startMode == (IODisplayModeID) kIODisplayModeIDBootProgrammable)
        || (0 == (startFlags & (kDisplayModeValidFlag | kMirrorOnlyFlags))))
    {
        // go to default
        if (connectRef->startMode != kIODisplayModeIDInvalid)
        {
            startMode  = connectRef->startMode;
            startDepth = connectRef->startDepth;
        }
        else if( connectRef->defaultMode)
        {
            startMode = connectRef->defaultMode;
            startDepth = connectRef->defaultDepth;
        }
        if( connectRef->make4By3 && connectRef->default4By3Mode)
            startMode = connectRef->default4By3Mode;
    }

    if ((startMode == mode) && (startDepth == depth))
    {
        startMode = kIODisplayModeIDCurrent;
    }
        
    DEBG(connectRef, "setMode %x, %d from %x, %d\n", 
            (int) startMode, (int) startDepth, (int) mode, (int) depth);
    IOFBSetDisplayModeAndDepth( connectRef->connect, startMode, startDepth );

    if (kIODisplayModeIDCurrent != startMode)
    {
        IOFBSetStartupDisplayModeAndDepth(connectRef->connect, startMode, startDepth);
    }

    return( kIOReturnSuccess );
}

kern_return_t
IOFBGetConnectState( io_connect_t connect, IOOptionBits * state )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if( !connectRef)
        return( kIOReturnBadArgument );

    *state = connectRef->state;

    DEBG(connectRef, "IOFBGetConnectState 0x%x\n", connectRef->state);

    return( kIOReturnSuccess );
}


// Mask of pixel formats available in mode and depth

kern_return_t
IOFBGetPixelFormats( io_connect_t connect __unused,
        IODisplayModeID         __unused displayMode,
        IOIndex                 __unused depth,
        UInt32 *                mask )
{
    *mask = 1;
    return( kIOReturnSuccess);
}

kern_return_t
__IOFBGetPixelInformation(
        IOFBConnectRef          connectRef,
        IODisplayModeID         displayMode,
        IOIndex                 depth,
        IOPixelAperture         aperture,
        IOPixelInformation *    pixelInfo )
{
    kern_return_t kr;

    uint64_t inData[] = { displayMode, depth, aperture };
    size_t len = sizeof( IOPixelInformation);
    kr = IOConnectCallMethod(connectRef->connect, 1,    // Index
            inData, arrayCnt(inData), NULL,    0,       // Input
            NULL,   NULL,             pixelInfo, &len); // Output

    if (kIOFBSwapAxes & connectRef->transform) 
    {
        UInt32 width = pixelInfo->activeWidth;
        pixelInfo->activeWidth = pixelInfo->activeHeight;
        pixelInfo->activeHeight = width;
//              pixelInfo->bytesPerRow = pixelInfo->activeHeight * 4;
    }

    return( kr );
}

kern_return_t
_IOFBGetPixelInformation(
        IOFBConnectRef          connectRef,
        IODisplayModeID         displayMode,
        IOIndex                 depth,
        IOPixelAperture         aperture,
        IOPixelInformation *    pixelInfo )
{
    kern_return_t  kr;

    if (displayMode == kIOFBSWOfflineDisplayModeID)
    {
        bzero(pixelInfo, sizeof(IOPixelInformation));
        pixelInfo->bytesPerRow = 32;
        pixelInfo->bytesPerPlane = 0;
        pixelInfo->flags = 0;
        pixelInfo->activeWidth = 1;
        pixelInfo->activeHeight = 1;
        strlcpy(pixelInfo->pixelFormat, "--------RRRRRRRRGGGGGGGGBBBBBBBB", 
        		sizeof(pixelInfo->pixelFormat));
        pixelInfo->pixelType = kIORGBDirectPixels;
        pixelInfo->componentMasks[0] = 0x00ff0000;
        pixelInfo->componentMasks[1] = 0x0000ff00;
        pixelInfo->componentMasks[2] = 0x000000ff;
        pixelInfo->bitsPerPixel = 32;
        pixelInfo->componentCount = 3;
        pixelInfo->bitsPerComponent = 8;

        return (kIOReturnSuccess);
    }

    kr = __IOFBGetPixelInformation(connectRef, displayMode, depth, aperture, pixelInfo);

    return( kr );
}

kern_return_t
IOFBGetPixelInformation( io_connect_t connect,
        IODisplayModeID         displayMode,
        IOIndex                 depth,
        IOPixelAperture         aperture,
        IOPixelInformation *    pixelInfo )
{
    IOFBConnectRef  connectRef;
    CFDictionaryRef dict;
    CFDataRef       data = NULL;
    size_t          offset;

    connectRef = IOFBConnectToRef(connect);
    if (!connectRef)
        return( kIOReturnBadArgument );

    dict = CFDictionaryGetValue( connectRef->modes, (const void *) (uintptr_t) (UInt32) displayMode );
    if (dict && !(data = CFDictionaryGetValue(dict, CFSTR(kIOFBModePIKey))))
    {
		IODisplayModeInformation modeInfo;
		_IOFBGetDisplayModeInformation(connectRef, displayMode, &modeInfo);
		data = CFDictionaryGetValue(dict, CFSTR(kIOFBModePIKey));	
	}
    if (!data)
    {
        return (_IOFBGetPixelInformation(connectRef, displayMode, depth,
                                               aperture, pixelInfo));
    }

    if (kIOFBSystemAperture != aperture)
        return (kIOReturnBadArgument);

    offset = depth * sizeof(IOPixelInformation);
    if ((offset + sizeof(IOPixelInformation)) > (size_t) CFDataGetLength(data))
        return (kIOReturnBadArgument);

    CFDataGetBytes(data, CFRangeMake(offset, sizeof(IOPixelInformation)), 
                        (UInt8 *) pixelInfo);

    return (kIOReturnSuccess);
}

kern_return_t
IOFBSetDisplayModeAndDepth( io_connect_t connect,
        IODisplayModeID         displayMode,
        IOIndex                 depth )
{
    kern_return_t  err;
    IOFBConnectRef connectRef;

	if (kIOFBSWOfflineDisplayModeID == displayMode)
    {
        return (kIOReturnSuccess);
    }

    connectRef = IOFBConnectToRef(connect);
    if( !connectRef)
        return( kIOReturnBadArgument );

    DEBG(connectRef, "setMode %x, %d \n", (int) displayMode, (int) depth);

    uint64_t inData[] = { displayMode, depth };
    err = IOConnectCallMethod(connect, 4,               // Index
            inData, arrayCnt(inData), NULL,    0,       // Input
            NULL,   NULL,             NULL, NULL);      // Output
                    
    DEBG(connectRef, "did setMode(%x)\n", err);

    if (kIOReturnSuccess == err)
    {
        IOFBResetTransform( connectRef );
        _IOFBGetPixelInformation(connectRef, displayMode, depth, kIOFBSystemAperture, &connectRef->currentModePixelInfo);
    }

    return (err);
}

kern_return_t
IOFBSetStartupDisplayModeAndDepth( io_connect_t connect,
        IODisplayModeID         displayMode,
        IOIndex                 depth )
{
    uint64_t inData[] = { displayMode, depth };

    if (kIOFBSWOfflineDisplayModeID == displayMode)
        return (kIOReturnSuccess);

    return IOConnectCallMethod(connect, 3,              // Index
            inData, arrayCnt(inData), NULL,    0,       // Input
            NULL,   NULL,             NULL, NULL);      // Output
}

kern_return_t
IOFBSetNewCursor( io_connect_t connect,
        void *                  cursor,
        IOIndex                 frame,
        IOOptionBits            options )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if( !connectRef)
        return( kIOReturnBadArgument );

    if (kIOFBConnectStateUnusable & connectRef->state)
       return( kIOReturnBadArgument );

    uint64_t inData[] = { (uintptr_t) cursor, frame, options };
    return IOConnectCallMethod(connect, 10,             // Index
            inData, arrayCnt(inData), NULL,    0,       // Input
            NULL,   NULL,             NULL, NULL);      // Output
}

kern_return_t
IOFBSetCursorVisible( io_connect_t connect,
        int                     visible )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if( !connectRef)
        return( kIOReturnBadArgument );

    if (kIOFBConnectStateUnusable & connectRef->state)
       return( kIOReturnSuccess );

    uint64_t inData[] = { visible };
    return IOConnectCallMethod(connect, 12,             // Index
            inData, arrayCnt(inData), NULL,    0,       // Input
            NULL,   NULL,             NULL, NULL);      // Output
}

kern_return_t
IOFBSetCursorPosition( io_connect_t connect,
        long int                x,
        long int                y )
{
    IOFBConnectRef connectRef = IOFBConnectToRef( connect );

    if( !connectRef)
        return( kIOReturnBadArgument );

    if (kIOFBConnectStateUnusable & connectRef->state)
       return( kIOReturnSuccess );

    uint64_t inData[] = { x, y };
    return IOConnectCallMethod(connect, 13,             // Index
            inData, arrayCnt(inData), NULL,    0,       // Input
            NULL,   NULL,             NULL, NULL);      // Output
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

CFDictionaryRef
IOFBCreateModeInfoDictionary(
        io_service_t            framebuffer __unused,
        IOOptionBits            options __unused,
        IODisplayModeID displayMode __unused,
        IODisplayModeInformation *      info)
{
    CFMutableDictionaryRef dict;
    CFStringRef            string;
    char                   buffer[128];

    dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                     &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
    if (!dict)
        return (dict);
    
    snprintf(buffer, sizeof(buffer), "%d x %d", (int) info->nominalWidth, (int) info->nominalHeight);
    string = CFStringCreateWithCString(kCFAllocatorDefault, buffer,
                                    kCFStringEncodingMacRoman);
    if (string)
    {
        CFDictionarySetValue(dict, CFSTR(kIOFBModeResolutionNameKey), string);
        CFRelease(string);
    }

    snprintf(buffer, sizeof(buffer), "%f Hertz", ((float) info->refreshRate) / 65536.0);
    string = CFStringCreateWithCString( kCFAllocatorDefault, buffer,
                                        kCFStringEncodingMacRoman);
    if (string)
    {
        CFDictionarySetValue(dict, CFSTR(kIOFBModeRefreshNameKey), string);
        CFRelease(string);
    }

    return (dict);
}


CFDictionaryRef
IOFBCreateDisplayModeDictionary( io_service_t framebuffer,
        IODisplayModeID         displayMode )
{
    CFDictionaryRef     infoDict;
    CFStringRef         string;
    CFDictionaryRef     modeDict = 0;
    char                keyBuf[12];

    infoDict = IORegistryEntryCreateCFProperty( framebuffer, CFSTR(kIOFramebufferInfoKey),
                                                kCFAllocatorDefault, kNilOptions );
    if( infoDict ) {
        snprintf(keyBuf, sizeof(keyBuf), "%x", (unsigned) displayMode );
        string = CFStringCreateWithCString( kCFAllocatorDefault, keyBuf,
                                            kCFStringEncodingMacRoman );
        if( string) {
            modeDict = CFDictionaryGetValue( infoDict, string );
            CFRelease( string );
        }
        if( modeDict)
            CFRetain( modeDict );
        CFRelease( infoDict );
    }

    return( modeDict );
}

CFDictionaryRef
IOFBGetPixelInfoDictionary( 
        CFDictionaryRef         modeDictionary,
        IOIndex                 depth,
        IOPixelAperture         aperture )
{
    char                keyBuf[12];
    CFStringRef         string;
    CFDictionaryRef     pixelInfo = 0;

    if( !modeDictionary)
        return( 0 );

    snprintf(keyBuf, sizeof(keyBuf), "%dx", (int) (depth + (aperture << 16)) );
    string = CFStringCreateWithCString( kCFAllocatorDefault, keyBuf,
                                        kCFStringEncodingMacRoman );
    if( string) {
        pixelInfo = CFDictionaryGetValue( modeDictionary, string );
        CFRelease( string );
    }

    return( pixelInfo );
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn
IOFBGetInterruptSemaphore( io_connect_t connect,
                           IOSelect interruptType,
                           semaphore_t * semaphore )
{
    uint64_t inData = interruptType;
    uint64_t outData = 0;
    uint32_t outCnt  = 1;
    return IOConnectCallMethod(connect, 15,     // Index
            &inData,  1,       NULL,    0,      // Input
            &outData, &outCnt, NULL, NULL);     // Output
    *semaphore = (semaphore_t) outData;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <IOKit/graphics/IOGraphicsInterface.h>

#ifdef NO_CFPLUGIN

struct _BlitterVars {
    IOGraphicsAcceleratorInterface ** interface;
    IOBlitterPtr                copyProc;
    IOBlitterPtr                fillProc;
    IOBlitterPtr                memCopyProc;
    IOBlitSurface               dest;
    void *                      sid;
    IOBlitterPtr                copyRegionProc;
};
typedef struct _BlitterVars _BlitterVars;

kern_return_t
IOPSAllocateBlitEngine( io_service_t service,
                void ** blitterRef, int * quality)
{
    IOReturn                            err = kIOReturnSuccess;
    _BlitterVars *                      vars;
    IOGraphicsAcceleratorInterface **   interface = 0;
    
    vars = (_BlitterVars *) calloc( 1, sizeof( _BlitterVars ));
    if( !vars)
        return( kIOReturnNoMemory);

    do {
        err = IOCreatePlugInInterfaceForService( service,
                            kIOGraphicsAcceleratorTypeID,
                            kIOGraphicsAcceleratorInterfaceID,
                            (IOCFPlugInInterface ***)&interface, (SInt32 *) quality );
        if( err)
            continue;
        vars->interface = interface;

        if( (*interface)->SetDestination) {
            err = (*interface)->SetDestination(interface,
                                        kIOBlitFramebufferDestination, NULL);
            if( err)
                continue;
        }
        err = (*interface)->GetBlitter(interface,
                                    kIOBlitAllOptions,
                                    (kIOBlitTypeCopyRects | kIOBlitCopyOperation),
                                    kIOBlitSourceDefault,
                                    &vars->copyProc);
        if( err)
            continue;
        err = (*interface)->GetBlitter(interface,
                                    kIOBlitAllOptions,
                                    (kIOBlitTypeRects | kIOBlitCopyOperation),
                                    kIOBlitSourceSolid,
                                    &vars->fillProc);
        if( err)
            continue;


        if( kIOReturnSuccess != (*interface)->GetBlitter(interface,
                                    kIOBlitAllOptions,
                                    (kIOBlitTypeCopyRegion | kIOBlitTypeOperationType0),
                                    kIOBlitSourceFramebuffer,
                                    &vars->copyRegionProc))
            vars->copyRegionProc = 0;

        if( kIOReturnSuccess != (*interface)->GetBlitter(interface,
                                    kIOBlitAllOptions,
                                    (kIOBlitTypeCopyRects | kIOBlitCopyOperation),
                                    kIOBlitSourceMemory,
                                    &vars->memCopyProc))
            vars->memCopyProc = 0;
        
    } while( FALSE );

    if( err) {
        if (interface)
            IODestroyPlugInInterface((IOCFPlugInInterface **)interface);
        free( vars );
        vars = 0;
    }
    
    *blitterRef = (void *) vars;

    return( err);
}

kern_return_t
IOPSBlitReset( void * blitterRef)
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface ** interface = vars->interface;
    kern_return_t       err = kIOReturnSuccess;

    if( interface) {
        if( vars->sid) {
            err = (*interface)->SetDestination(interface, kIOBlitFramebufferDestination, 0);
        }
        err = (*interface)->Reset(interface, kNilOptions);
    }
    vars->sid = 0;
    return( err );
}

kern_return_t
IOPSBlitDeallocate( void * blitterRef)
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface ** interface = vars->interface;
    kern_return_t       err;

    err = IODestroyPlugInInterface((IOCFPlugInInterface **)interface);
    free( vars );

    return( err );
}

kern_return_t
IOPSBlitIdle( void * blitterRef)
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface ** interface = vars->interface;
    kern_return_t       err;

    err = (*interface)->WaitComplete(interface, kIOBlitWaitAll2D );

    return( err );
}


kern_return_t
IOFBSynchronize( void * blitterRef,
                UInt32 x, UInt32 y, UInt32 w, UInt32 h, UInt32 options )
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface ** interface;
    IOReturn            err;

    if( !vars)
        return( kIOReturnBadArgument);
    interface = vars->interface;
    err = (*interface)->Synchronize(interface, options, x, y, w, h );

    return( err );
}

kern_return_t
IOFBBeamPosition( void * blitterRef, UInt32 options, SInt32 * position )
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface ** interface = vars->interface;
    IOReturn            err;

    err = (*interface)->GetBeamPosition(interface, options, position);

    return( err );
}


kern_return_t
IOPSBlitFill( void * blitterRef,
                int x, int y, int w, int h, int data )
{
    _BlitterVars *              vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface ** interface = vars->interface;
    IOReturn                    err;
    IOBlitRectangles            rects;

    if( vars->sid) {
        err = (*interface)->SetDestination(interface, kIOBlitFramebufferDestination, 0);
        vars->sid = 0;
        if( err)
            return( err );
    }

    rects.count = 1;
    rects.rects[0].x = x;
    rects.rects[0].y = y;
    rects.rects[0].width = w;
    rects.rects[0].height = h;

    err = (*vars->fillProc)(interface,
                    kNilOptions,
                    (kIOBlitTypeRects | kIOBlitCopyOperation),
                    (kIOBlitSourceSolid | kIOBlitDestFramebuffer),
                    &rects.operation,
                    (void *) (uintptr_t) data);
    
    if( kIOReturnSuccess == err)
        (*interface)->Flush(interface, kNilOptions);

    return( err );
}

kern_return_t
IOPSBlitInvert( void * blitterRef,
                int x, int y, int w, int h )
{
    _BlitterVars *              vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface ** interface = vars->interface;
    IOReturn                    err;
    IOBlitRectangles            rects;

    if( vars->sid) {
        err = (*interface)->SetDestination(interface, kIOBlitFramebufferDestination, 0);
        vars->sid = 0;
        if( err)
            return( err );
    }

    rects.count = 1;
    rects.rects[0].x = x;
    rects.rects[0].y = y;
    rects.rects[0].width = w;
    rects.rects[0].height = h;

    err = (*vars->fillProc)(interface,
                    kNilOptions,
                    (kIOBlitTypeRects | kIOBlitCopyOperation),
                    (kIOBlitSourceSolid | kIOBlitDestFramebuffer),
                    &rects.operation,
                    (void *) 0xffffffff);
    
    if( kIOReturnSuccess == err)
        (*interface)->Flush(interface, kNilOptions);

    return( err );
}


kern_return_t
IOPSBlitCopy( void * blitterRef,
                int src_x, int src_y, int width, int height,
                int dst_x, int dst_y )
{
    return( IOFBBlitVRAMCopy( blitterRef, src_x, src_y, width, height,
                                dst_x, dst_y, 1 * (kIOFBBlitBeamSync) ));
}

kern_return_t
IOFBBlitVRAMCopy( void * blitterRef,
                  int sourceX, int sourceY, int width, int height,
                  int x, int y, IOOptionBits options )
{
    _BlitterVars *                      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface **   interface = vars->interface;
    IOReturn                            err;
    IOBlitCopyRectangles                rects;

    if( vars->sid) {
        err = (*interface)->SetDestination(interface, kIOBlitFramebufferDestination, 0);
        vars->sid = 0;
        if( err)
            return( err );
    }

    rects.count = 1;
    rects.rects[0].x = x;
    rects.rects[0].y = y;
    rects.rects[0].width = width;
    rects.rects[0].height = height;
    rects.rects[0].sourceX = sourceX;
    rects.rects[0].sourceY = sourceY;

    err = (*vars->copyProc)(interface,
                    options,
                    (kIOBlitTypeCopyRects | kIOBlitCopyOperation),
                    kIOBlitSourceDefault,
                    &rects.operation,
                    0);
    
    if( kIOReturnSuccess == err)
        (*interface)->Flush(interface, kNilOptions);

    return( err );
}

kern_return_t
IOFBBlitSurfaceCopy( void * blitterRef, IOOptionBits options, void * surfaceID,
                     IOAccelDeviceRegion * region, UInt32 surfaceX, UInt32 surfaceY )
{
    IOReturn                            err = kIOReturnSuccess;
    _BlitterVars *                      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface **   interface = vars->interface;
    IOBlitCopyRegion                    op;

    if( 0 == vars->copyRegionProc)
        return( kIOReturnUnsupported );

    if( surfaceID != vars->sid) do {
        if( surfaceID) {
            if (vars->dest.interfaceRef)
                (*interface)->FreeSurface(interface, kIOBlitHasCGSSurface, &vars->dest);
            err = (*interface)->AllocateSurface(interface, kIOBlitHasCGSSurface, &vars->dest, surfaceID);
            if( err)
                continue;
            err = (*interface)->SetDestination(interface, kIOBlitSurfaceDestination, &vars->dest);
        } else
            err = (*interface)->SetDestination(interface, kIOBlitFramebufferDestination, 0);

        if( err)
            continue;

        vars->sid = surfaceID;

    } while( false );

    if( err)
        return( err );

    op.region = region;
    op.deltaX = surfaceX;
    op.deltaY = surfaceY;

    err = (*vars->copyRegionProc)(interface,
                    options,
                    (kIOBlitTypeCopyRegion | kIOBlitTypeOperationType0),
                    kIOBlitSourceFramebuffer,
                    &op.operation,
                    (void *) 0);

    if( kIOReturnSuccess == err)
        (*interface)->Flush(interface, kNilOptions);

    return( err );
}

kern_return_t
IOFBBlitSurfaceSurfaceCopy( void * blitterRef, IOOptionBits options,
                            void * sourceSurfaceID, void * destSurfaceID,
                            IOAccelDeviceRegion * region, UInt32 surfaceX, UInt32 surfaceY )
{
    IOReturn                            err = kIOReturnSuccess;
    _BlitterVars *                      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface **   interface = vars->interface;
    IOBlitCopyRegion                    op;

    if( 0 == vars->copyRegionProc)
        return( kIOReturnUnsupported );

    if( destSurfaceID != vars->sid) do {
        if( destSurfaceID) {
            if (vars->dest.interfaceRef)
                (*interface)->FreeSurface(interface, kIOBlitHasCGSSurface, &vars->dest);
            err = (*interface)->AllocateSurface(interface, kIOBlitHasCGSSurface, &vars->dest, destSurfaceID);
            if( err)
                continue;
            err = (*interface)->SetDestination(interface, kIOBlitSurfaceDestination, &vars->dest);
        } else
            err = (*interface)->SetDestination(interface, kIOBlitFramebufferDestination, 0);

        if( err)
            continue;

        vars->sid = destSurfaceID;

    } while( false );

    if( err)
        return( err );

    op.region = region;
    op.deltaX = surfaceX;
    op.deltaY = surfaceY;

    err = (*vars->copyRegionProc)(interface,
                    options,
                    (kIOBlitTypeCopyRegion | kIOBlitTypeOperationType0),
                    kIOBlitSourceCGSSurface,
                    &op.operation,
                    (void *) sourceSurfaceID);

    if( kIOReturnSuccess == err)
        (*interface)->Flush(interface, kNilOptions);

    return( err );
}

#if 0

kern_return_t
IOFBSetupFIFOBurst( void * blitterRef,
                UInt32 x, UInt32 y, UInt32 w, UInt32 h,
                UInt32 options, void ** burstRef )
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOReturn            err;
    boolean_t           wait;

    do {
        IOSharedLockLock( &vars->context->contextLock );
        wait = (kIOReturnBusy == (
                err = vars->procs.setupFIFOBurst( vars->chipRef, x, y, w, h,
                                                        options, burstRef )));
        IOSharedLockUnlock( &vars->context->contextLock, wait );
    } while( wait );

    return( err );
}

kern_return_t
IOFBCommitMemory( void * blitterRef,
                vm_address_t start, vm_size_t length, IOOptionBits options,
                void ** memoryRef, IOByteCount * offset )
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOReturn            err;
    unsigned int        len;
    int                 params[ 3 ];

    params[0] = start;
    params[1] = length;
    params[2] = options;
    len = 2;
    err = io_connect_method_scalarI_scalarO( vars->connect, 2, /*index*/
                    params, 3, params, &len);

    if( kIOReturnSuccess == err) {
        *memoryRef = (void *) params[0];    
        *offset = params[1];    
    }

    return( err );
}

kern_return_t
IOFBReleaseMemory( void * blitterRef, void * memoryRef )
{
    _BlitterVars *      vars = (_BlitterVars *) blitterRef;
    IOReturn            err;
    unsigned int        len;

    IOPSBlitIdle( blitterRef );

    len = 0;
    err = io_connect_method_scalarI_scalarO( vars->connect, 3, /*index*/
                    (int *) &memoryRef, 1, NULL, &len);

    return( err );
}

#endif

kern_return_t
IOFBMemoryCopy( void * blitterRef,
                        UInt32 x, UInt32 y,
                        UInt32 width, UInt32 height, 
                        UInt32 srcByteOffset, UInt32 srcRowBytes,
                        SInt32 * token __unused)
{
    _BlitterVars *                      vars = (_BlitterVars *) blitterRef;
    IOGraphicsAcceleratorInterface **   interface = vars->interface;
    IOReturn                            err;
    IOBlitMemory                        source;
    IOBlitCopyRectangles                rects;

    if( vars->sid) {
        err = (*interface)->SetDestination(interface, kIOBlitFramebufferDestination, 0);
        vars->sid = 0;
        if( err)
            return( err );
    }

    rects.count = 1;
    rects.rects[0].x = x;
    rects.rects[0].y = y;
    rects.rects[0].width = width;
    rects.rects[0].height = height;
    rects.rects[0].sourceX = 0;
    rects.rects[0].sourceY = 0;

    source.memory.ref = 0;      // !!
    source.byteOffset = srcByteOffset;
    source.rowBytes = srcRowBytes;

    err = (*vars->memCopyProc)(interface,
                    kNilOptions,
                    (kIOBlitTypeCopyRects | kIOBlitCopyOperation),
                    kIOBlitSourceMemory,
                    &rects.operation,
                    (void *) &source);

    return( err );
}

#else /* NO_CFPLUGIN */

/* We need these symbols to exist to prevent link errors in clients.  Have them all return an error. */

kern_return_t
IOPSAllocateBlitEngine( io_connect_t framebuffer, void ** blitterRef, int * quality)
{ return kIOReturnUnsupported; }

kern_return_t
IOPSBlitReset( void * blitterRef)
{ return kIOReturnUnsupported; }

kern_return_t
IOPSBlitDeallocate( void * blitterRef )
{ return kIOReturnUnsupported; }

kern_return_t
IOPSBlitIdle( void * blitterRef )
{ return kIOReturnUnsupported; }

kern_return_t
IOFBWaitForCompletion( void * blitterRef, SInt32 token )
{ return kIOReturnUnsupported; }

kern_return_t
IOFBSynchronize( void * blitterRef, UInt32 x, UInt32 y, UInt32 w, UInt32 h, UInt32 options )
{ return kIOReturnUnsupported; }
     
kern_return_t
IOFBBeamPosition( void * blitterRef, UInt32 options, SInt32 * position )
{ return kIOReturnUnsupported; }
     
kern_return_t
IOPSBlitFill( void * blitterRef, int dst_x, int dst_y, int width, int height, int data )
{ return kIOReturnUnsupported; }

kern_return_t
IOPSBlitInvert( void * blitterRef, int x, int y, int w, int h )
{ return kIOReturnUnsupported; }

kern_return_t
IOPSBlitCopy( void * blitterRef, int src_x, int src_y, int width, int height, int dst_x, int dst_y )
{ return kIOReturnUnsupported; }

kern_return_t
IOFBBlitVRAMCopy( void * blitterRef, int sourceX, int sourceY, int width, int height, int x, int y, IOOptionBits options )
{ return kIOReturnUnsupported; }

kern_return_t
IOFBMemoryCopy( void * blitterRef, UInt32 x, UInt32 y, UInt32 width, UInt32 height, UInt32 srcByteOffset, UInt32 srcRowBytes, SInt32 * token)
{ return kIOReturnUnsupported; }

kern_return_t
IOFBSetupFIFOBurst( void * blitterRef, UInt32 x, UInt32 y, UInt32 w, UInt32 h, UInt32 options, void ** burstRef )
{ return kIOReturnUnsupported; }

void
IOFBBurstWrite32( void * p1, void * p2, void * p3, void * p4, void * p5, void * p6, void * p7, void * p8 )
{ return; }

void
IOFBSetBurstRef( void * burstRef )
{ return; }

kern_return_t
IOFBCommitMemory( void * blitterRef, vm_address_t start, vm_size_t length, IOOptionBits options, void ** memoryRef, IOByteCount * offset )
{ return kIOReturnUnsupported; }

kern_return_t
IOFBReleaseMemory( void * blitterRef, void * memoryRef )
{ return kIOReturnUnsupported; }

#endif /* !NO_CFPLUGIN */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <IOKit/i2c/IOI2CInterfacePrivate.h>

struct IOI2CConnect
{
    io_connect_t connect;
};


IOReturn IOI2CCopyInterfaceForID( CFTypeRef identifier, io_service_t * interface )
{
    CFMutableDictionaryRef dict, matching;
    mach_port_t            masterPort;
    kern_return_t          kr;
    io_iterator_t          iter;

    IOMasterPort( MACH_PORT_NULL, &masterPort );
    
    matching = IOServiceMatching(kIOI2CInterfaceClassName);
    if(!matching)
        return( kIOReturnNoMemory );

    dict = CFDictionaryCreateMutable( kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
    if(!dict)
        return( kIOReturnNoMemory );

    CFDictionarySetValue(dict, CFSTR(kIOI2CInterfaceIDKey), identifier);
    CFDictionarySetValue(matching, CFSTR(kIOPropertyMatchKey), dict);
    CFRelease(dict);

    kr = IOServiceGetMatchingServices( masterPort, matching, &iter);
    if( kIOReturnSuccess == kr) {
        *interface = IOIteratorNext( iter );
        IOObjectRelease( iter );
    }

    return( kr );
}

IOReturn IOFBGetI2CInterfaceCount( io_service_t framebuffer, IOItemCount * count )
{
    CFArrayRef   array;

    array = IORegistryEntryCreateCFProperty( framebuffer, CFSTR(kIOFBI2CInterfaceIDsKey),
                                             kCFAllocatorDefault, kNilOptions );
    if( array) {
        *count = CFArrayGetCount(array);
        CFRelease( array );
    } else
        *count = 0;

    return( kIOReturnSuccess );
}

IOReturn IOFBCopyI2CInterfaceForBus( io_service_t framebuffer, IOOptionBits bus, io_service_t * interface )
{
    IOReturn     kr = kIOReturnNoDevice;
    CFArrayRef   array;
    CFIndex      index;
    CFTypeRef    ident;

    array = IORegistryEntryCreateCFProperty( framebuffer, CFSTR(kIOFBI2CInterfaceIDsKey),
                                             kCFAllocatorDefault, kNilOptions );
    if( !array)
        return( kIOReturnNoDevice );

    index = bus & kIOI2CBusNumberMask;

    do {
        if( index >= CFArrayGetCount(array)) {
            kr = kIOReturnNoDevice;
            continue;
        }

        ident = CFArrayGetValueAtIndex(array, index);
        kr = IOI2CCopyInterfaceForID( ident, interface );

    } while( false );

    CFRelease( array );

    return( kr );
}

IOReturn IOI2CInterfaceOpen( io_service_t interface, IOOptionBits options,
                             IOI2CConnectRef * connect )
{
    kern_return_t         kr;
    struct IOI2CConnect * connectRef;

    if( !IOObjectConformsTo(interface, kIOI2CInterfaceClassName))
        return( kIOReturnBadArgument );

    connectRef = calloc(1, sizeof(struct IOI2CConnect));
    if( !connectRef)
        return( kIOReturnNoMemory );

    kr = IOServiceOpen( interface, mach_task_self(), options, &connectRef->connect );

    if( (kr != kIOReturnSuccess) && connectRef) {
        free(connectRef);
        connectRef = NULL;
    }
    *connect = connectRef;

    return( kr );
}

IOReturn IOI2CInterfaceClose( IOI2CConnectRef connect, IOOptionBits options __unused )
{
    kern_return_t kr;

    kr = IOServiceClose( connect->connect );

    free( connect );

    return( kr );
}

IOReturn IOI2CSendRequest( IOI2CConnectRef connect, IOOptionBits options __unused, 
                           IOI2CRequest * request )
{
    kern_return_t kr;
    IOI2CBuffer   buffer;

    if( request->sendBytes > sizeof(buffer.inlineBuffer))
        return( kIOReturnOverrun );
    if( request->replyBytes > sizeof(buffer.inlineBuffer))
        return( kIOReturnOverrun );

    kr = IOConnectCallMethod(connect->connect, 0,       // Index
                NULL,    0, NULL,    0,                 // Input
                NULL, NULL, NULL, NULL);                // Output
    if( kIOReturnSuccess != kr)
        return( kr );

    buffer.request = *request;
    buffer.request.replyBuffer = 0;
    buffer.request.sendBuffer  = 0;

    if( request->sendBytes)
        bcopy( (void *) request->sendBuffer, &buffer.inlineBuffer[0], request->sendBytes );

    size_t len = sizeof( buffer);
    kr = IOConnectCallMethod(connect->connect, 2,       // Index
                NULL,    0, &buffer, len,               // Input
                NULL, NULL, &buffer, &len);             // Output

    if( buffer.request.replyBytes)
        bcopy( &buffer.inlineBuffer[0], (void *)  request->replyBuffer, buffer.request.replyBytes );
    *request = buffer.request;

    return IOConnectCallMethod(connect->connect, 1,     // Index
                NULL,    0, NULL,    0,                 // Input
                NULL, NULL, NULL, NULL);                // Output
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

