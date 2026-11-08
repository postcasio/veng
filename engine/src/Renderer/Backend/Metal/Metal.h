#pragma once

// The Metal backend's common include: the framework, and the small conversions every unit shares.
// Objective-C++ with ARC; included only by the backend's own .mm sources.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <Veng/Assert.h>
#include <Veng/Veng.h>

// The runtime's autorelease pool entry points, the calls @autoreleasepool compiles to; declared here
// because a recording opens its pool in one call and drains it in another.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace Veng::Renderer::Metal
{
    /// @brief Converts engine text to an NSString.
    /// @param text The UTF-8 text.
    /// @return The string, autoreleased.
    inline NSString* ToNSString(const string_view text)
    {
        return [[NSString alloc] initWithBytes:text.data()
                                        length:text.size()
                                      encoding:NSUTF8StringEncoding];
    }

    /// @brief Converts an NSString to engine text.
    /// @param text The string, or nil.
    /// @return The UTF-8 text; empty for nil.
    inline string ToString(NSString* text)
    {
        if (text == nil)
        {
            return {};
        }
        return string([text UTF8String]);
    }

    /// @brief Describes an error for a log line: its description and every detail it carries.
    /// @param error The error, or nil.
    /// @return The description; empty for nil.
    inline string Describe(NSError* error)
    {
        if (error == nil)
        {
            return {};
        }
        string text = ToString([error localizedDescription]);
        for (NSString* key in [error userInfo])
        {
            if ([key isEqualToString:NSLocalizedDescriptionKey])
            {
                continue;
            }
            text += fmt::format("; {}: {}", ToString(key),
                                ToString([[[error userInfo] objectForKey:key] description]));
        }
        return text;
    }
}
