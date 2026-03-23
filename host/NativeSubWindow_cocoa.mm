/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#import <QuartzCore/CALayer.h>
#import <QuartzCore/CAMetalLayer.h>


#include "NativeSubWindow.h"
#include <Cocoa/Cocoa.h>

/*
 * EmuGLView inherit from NSView and override the isOpaque
 * method to return YES. That prevents drawing of underlying window/view
 * when the view needs to be redrawn.
 */
@interface EmuGLView : NSView {
} @end

@implementation EmuGLView

  - (BOOL)isOpaque {
      return YES;
  }

@end

@interface EmuGLViewWithMetal : NSView {
} @end

@implementation EmuGLViewWithMetal

  - (BOOL)isOpaque {
      return YES;
  }

  + (Class) layerClass {
    return [CAMetalLayer class];
  }

  - (CALayer *)makeBackingLayer {
    CALayer * layer = [CAMetalLayer layer];
    return layer;
  }

@end

void dispatch_main_safe(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

EGLNativeWindowType createSubWindow(FBNativeWindowType p_window,
                                    int x,
                                    int y,
                                    int width,
                                    int height,
                                    float dpr,
                                    SubWindowRepaintCallback repaint_callback,
                                    void* repaint_callback_param,
                                    int hideWindow) {
    NSWindow* win = (NSWindow *)p_window;
    if (!win) {
        return NULL;
    }

    // Enable views with metal backing when hardware acceleration is enabled, as it should provide
    // better performance and can be necessary for vulkan swapchain creation.
    bool useMetalView = false;
    const char* angle_default_platform = getenv("ANGLE_DEFAULT_PLATFORM");
    const char* emuVulkanICD = getenv("ANDROID_EMU_VK_ICD");
    if (angle_default_platform && 0 == strcmp("metal", angle_default_platform)) {
        // Legacy behavior: ANGLE on Metal requires a Metal-backed view.
        useMetalView = true;
    }
    if (emuVulkanICD) {
        if ((0 == strcmp("swiftshader", emuVulkanICD)) ||
            (0 == strcmp("lavapipe", emuVulkanICD))) {
            // Do not enable metal views when using software rendering.
            useMetalView = false;
        } else {
            // Hardware Vulkan swapchains on macOS require a CAMetalLayer even when
            // ANGLE_DEFAULT_PLATFORM is unset.
            useMetalView = true;
        }
    }

    __block EGLNativeWindowType subwin = NULL;
    dispatch_main_safe(^{
        /* (x,y) assume an upper-left origin, but Cocoa uses a lower-left origin */
        NSRect content_rect = [win contentRectForFrameRect:[win frame]];
        int cocoa_y = (int)content_rect.size.height - (y + height);
        NSRect contentRect = NSMakeRect(x, cocoa_y, width, height);

        NSView* glView = NULL;
        if (useMetalView) {
            glView = [[EmuGLViewWithMetal alloc] initWithFrame:contentRect];
        } else {
            glView = [[EmuGLView alloc] initWithFrame:contentRect];
        }
        if (!glView) {
            return;
        }
        [glView setWantsBestResolutionOpenGLSurface:YES];
        [glView setWantsLayer:YES];
        [[win contentView] addSubview:glView];
        [win makeKeyAndOrderFront:nil];
        if (hideWindow) {
            [glView setHidden:YES];
        }
        // We cannot use the dpr from [NSScreen mainScreen], which usually
        // gives the wrong screen at this point.
        [glView.layer setContentsScale:dpr];
        subwin = (EGLNativeWindowType)(glView);
    });
    return subwin;
}

void destroySubWindow(EGLNativeWindowType win) {
    if(win){
        dispatch_main_safe(^{
            NSView *glView = (NSView *)win;
            [glView removeFromSuperview];
            [glView release];
        });
    }
}

int moveSubWindow(FBNativeWindowType p_parent_window,
                  EGLNativeWindowType p_sub_window,
                  int x,
                  int y,
                  int width,
                  int height,
                  float dpr) {
    NSWindow *win = (NSWindow *)p_parent_window;
    if (!win) {
        return 0;
    }

    NSView *glView = (NSView *)p_sub_window;
    if (!glView) {
        return 0;
    }

    __block int result = 0;
    dispatch_main_safe(^{
        /* (x,y) assume an upper-left origin, but Cocoa uses a lower-left origin */
        NSRect content_rect = [win contentRectForFrameRect:[win frame]];
        int cocoa_y = (int)content_rect.size.height - (y + height);
        NSRect newFrame = NSMakeRect(x, cocoa_y, width, height);

        if ([glView.layer isKindOfClass:[CAMetalLayer class]]) {
            // Keep Metal-backed views attached during live resize so the old
            // frame remains visible while gfxstream recreates the swapchain.
            [glView setFrame:newFrame];
            [glView.layer setContentsScale:dpr];
        } else {
            /* The legacy non-Metal path still expects a detach/reattach cycle. */
            [glView removeFromSuperview];
            [glView setFrame:newFrame];
            [[win contentView] addSubview:glView];
        }
        result = 1;
    });
    return result;
}
