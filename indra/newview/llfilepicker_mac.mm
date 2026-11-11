/**
 * @file llfilepicker_mac.cpp
 * @brief OS-specific file picker
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifdef LL_DARWIN
#import <Cocoa/Cocoa.h>
#include <iostream>
#include "llfilepicker_mac.h"

// <WolfViewer 2026-11-11> File names, extensions and paths cross between the viewer and Cocoa as UTF-8 both ways.
// Into Cocoa: the callers (llfilepicker.cpp doNavSaveDialog / doNavSaveDialogModeless / navOpenFilterProc) pass
// UTF-8 std::strings - a suggested name can carry an object's or snapshot's non-English name - and
// [NSString defaultCStringEncoding] (the legacy system encoding, MacRoman on an English macOS) turned every
// non-ASCII character into several wrong ones. stringWithCString:encoding: returns nil for bytes that are not valid
// UTF-8, so the old encoding stays as the fall-back (the same rule as llwindowmacosx-objc.mm wolfNSStringFromUTF8).
// Back to C++: [NSString UTF8String], as before - mFiles is used with LLFile / std::ifstream, which take UTF-8 POSIX
// paths; the precomposed form it gives names the same file as fileSystemRepresentation's decomposed one on APFS and
// HFS+, and is what the rest of the viewer shows and compares.
static NSString* wolfNSStringFromUTF8(const std::string& s)
{
    NSString* str = [NSString stringWithCString:s.c_str() encoding:NSUTF8StringEncoding];
    return str ? str : [NSString stringWithCString:s.c_str() encoding:[NSString defaultCStringEncoding]];
}

static std::string wolfUTF8FromNSString(NSString* str)
{
    const char* utf8 = str ? [str UTF8String] : nullptr;   // nil for a nil string; never nil for a file URL's path
    return utf8 ? std::string(utf8) : std::string();
}
// </WolfViewer>

NSOpenPanel *init_panel(const std::vector<std::string>* allowed_types, unsigned int flags)
{
    int i;

    NSOpenPanel *panel = [NSOpenPanel openPanel];
    NSMutableArray *fileTypes = nil;


    if ( allowed_types && !allowed_types->empty())
    {
        fileTypes = [[NSMutableArray alloc] init];

        for (i=0;i<allowed_types->size();++i)
        {
            [fileTypes addObject:wolfNSStringFromUTF8((*allowed_types)[i])];   // <WolfViewer 2026-11-11/> UTF-8
        }
    }

    //[panel setMessage:@"Import one or more files or directories."];
    [panel setAllowsMultipleSelection: ( (flags & F_MULTIPLE)?true:false ) ];
    [panel setCanChooseDirectories: ( (flags & F_DIRECTORY)?true:false ) ];
    [panel setCanCreateDirectories: true];
    [panel setResolvesAliases: true];
    [panel setCanChooseFiles: ( (flags & F_FILE)?true:false )];
    [panel setTreatsFilePackagesAsDirectories: ( flags & F_NAV_SUPPORT ) ];

    if (fileTypes)
    {
        [panel setAllowedFileTypes:fileTypes];
    }
    else
    {
        // I suggest it's better to open the last path and let this default to home dir as necessary
        // for consistency with other OS X apps
        //
        //[panel setDirectoryURL: fileURLWithPath(NSHomeDirectory()) ];
    }
    return panel;
}

std::unique_ptr<std::vector<std::string>> doLoadDialog(const std::vector<std::string>* allowed_types,
                 unsigned int flags)
{
    std::unique_ptr<std::vector<std::string>> outfiles;

    @autoreleasepool
    {
        int result;
        //Aura TODO:  We could init a small window and release it at the end of this routine
        //for a modeless interface.

        NSOpenPanel *panel = init_panel(allowed_types,flags);

        result = [panel runModal];

        if (result == NSModalResponseOK)
        {
            NSArray *filesToOpen = [panel URLs];
            int i, count = [filesToOpen count];

            if (count > 0)
            {
                outfiles.reset(new std::vector<std::string>);
            }

            for (i=0; i<count; i++) {
                outfiles->push_back(wolfUTF8FromNSString([[filesToOpen objectAtIndex:i] path]));   // <WolfViewer 2026-11-11/>
            }
        }
    }
    return outfiles;
}

void doLoadDialogModeless(const std::vector<std::string>* allowed_types,
                 unsigned int flags,
                 void (*callback)(bool, std::vector<std::string> &, void*),
                 void *userdata)
{

    @autoreleasepool
    {
        NSOpenPanel *panel = init_panel(allowed_types,flags);
        NSWindow *mainWindow = [NSApp mainWindow];

        if (mainWindow)
        {
            [panel beginSheetModalForWindow:mainWindow
                          completionHandler:^(NSModalResponse result)
            {
                std::vector<std::string> outfiles;
                if (result == NSModalResponseOK)
                {
                    NSArray *filesToOpen = [panel URLs];
                    int i, count = [filesToOpen count];

                    if (count > 0)
                    {

                        for (i=0; i<count; i++) {
                            // <WolfViewer 2026-11-11/> UTF-8 helper; was a heap std::string per file, never deleted
                            outfiles.push_back(wolfUTF8FromNSString([[filesToOpen objectAtIndex:i] path]));
                        }
                        callback(true, outfiles, userdata);
                    }
                    else // no valid result
                    {
                        callback(false, outfiles, userdata);
                    }
                }
                else // cancel
                {
                    callback(false, outfiles, userdata);
                }
            }];
        }
        else
        {
            //present as modeless window
            [panel beginWithCompletionHandler:^(NSModalResponse result)
            {
                std::vector<std::string> outfiles;
                if (result == NSModalResponseOK)
                {
                    NSArray *filesToOpen = [panel URLs];
                    int i, count = [filesToOpen count];

                    if (count > 0)
                    {

                        for (i=0; i<count; i++) {
                            // <WolfViewer 2026-11-11/> UTF-8 helper; was a heap std::string per file, never deleted
                            outfiles.push_back(wolfUTF8FromNSString([[filesToOpen objectAtIndex:i] path]));
                        }
                        callback(true, outfiles, userdata);
                    }
                    else // no valid result
                    {
                        callback(false, outfiles, userdata);
                    }
                }
                else // cancel
                {
                    callback(false, outfiles, userdata);
                }
            }];
        }
    }
}

std::unique_ptr<std::string> doSaveDialog(const std::string* file,
                  const std::string* type,
                  const std::string* creator,
                  const std::string* extension,
                  unsigned int flags)
{
    std::unique_ptr<std::string> outfile;
    @autoreleasepool
    {
        NSSavePanel *panel = [NSSavePanel savePanel];

        NSString *extensionns = wolfNSStringFromUTF8(*extension);   // <WolfViewer 2026-11-11/> UTF-8
        NSArray *fileType = [extensionns componentsSeparatedByString:@","];

        //[panel setMessage:@"Save Image File"];
        [panel setTreatsFilePackagesAsDirectories: ( flags & F_NAV_SUPPORT ) ];
        [panel setCanSelectHiddenExtension:true];
        [panel setAllowedFileTypes:fileType];
        NSString *fileName = wolfNSStringFromUTF8(*file);   // <WolfViewer 2026-11-11/> UTF-8

        NSURL* url = [NSURL fileURLWithPath:fileName];
        [panel setNameFieldStringValue: fileName];
        [panel setDirectoryURL: url];
        if([panel runModal] ==
           NSModalResponseOK)
        {
            NSURL* url = [panel URL];
            NSString* p = [url path];
            outfile.reset(new std::string(wolfUTF8FromNSString(p)));   // <WolfViewer 2026-11-11/>
            // write the file
        }
    }
    return outfile;
}

void doSaveDialogModeless(const std::string* file,
                  const std::string* type,
                  const std::string* creator,
                  const std::string* extension,
                  unsigned int flags,
                  void (*callback)(bool, std::string&, void*),
                  void *userdata)
{
    @autoreleasepool {
        NSSavePanel *panel = [NSSavePanel savePanel];

        NSString *extensionns = wolfNSStringFromUTF8(*extension);   // <WolfViewer 2026-11-11/> UTF-8
        NSArray *fileType = [extensionns componentsSeparatedByString:@","];

        //[panel setMessage:@"Save Image File"];
        [panel setTreatsFilePackagesAsDirectories: ( flags & F_NAV_SUPPORT ) ];
        [panel setCanSelectHiddenExtension:true];
        [panel setAllowedFileTypes:fileType];
        NSString *fileName = wolfNSStringFromUTF8(*file);   // <WolfViewer 2026-11-11/> UTF-8

        NSURL* url = [NSURL fileURLWithPath:fileName];
        [panel setNameFieldStringValue: fileName];

        NSURL *last_url = [[NSUserDefaults standardUserDefaults] URLForKey:@"NSNavLastRootDirectory"];
        if(!last_url)
        {
            NSURL *downloads_url = [[NSFileManager defaultManager] URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask].firstObject;
            [panel setDirectoryURL:downloads_url];
        }

        [panel beginWithCompletionHandler:^(NSModalResponse result)
        {
            if (result == NSModalResponseOK)
            {
                NSURL* url = [panel URL];
                NSString* p = [url path];
                std::string outfile(wolfUTF8FromNSString(p));   // <WolfViewer 2026-11-11/>

                callback(true, outfile, userdata);
            }
            else // cancel
            {
                std::string outfile;
                callback(false, outfile, userdata);
            }
        }];
    }
}

#endif
