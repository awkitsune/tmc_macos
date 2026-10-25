/*
 * iOS/iPadOS front end: one window scene showing the game (gameview.m), after
 * the first-launch ROM import (romimport.m) when the data is not there yet.
 */

#import <UIKit/UIKit.h>

#import "gameview.h"
#import "romimport.h"

#include "audio.h"
#include "frontend.h"
#include "latency.h"

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation SceneDelegate {
    GameViewController* _game;
}

- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.backgroundColor = UIColor.blackColor;
    // Builds without the game data built in get it from the player's ROM first.
    if ([RomImport loadCache]) {
        [self showGame];
    } else {
        __weak SceneDelegate* weakSelf = self;
        self.window.rootViewController = [[RomImportViewController alloc] initWithReady:^{
            [weakSelf showGame];
        }];
    }
    [self.window makeKeyAndVisible];
}

- (void)showGame {
    _game = [GameViewController new];
    self.window.rootViewController = _game;
}

- (void)sceneWillResignActive:(UIScene*)scene {
    [_game sceneWillResignActive];
}

- (void)sceneDidBecomeActive:(UIScene*)scene {
    [_game sceneDidBecomeActive];
}

- (void)sceneDidEnterBackground:(UIScene*)scene {
    [_game sceneDidEnterBackground];
}

- (void)sceneWillEnterForeground:(UIScene*)scene {
    [_game sceneWillEnterForeground];
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    return YES;
}

- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    config.delegateClass = SceneDelegate.class;
    return config;
}

- (void)applicationWillTerminate:(UIApplication*)application {
    FrontendStopGame();
    AudioStop();
    LatencyFinish();
}

@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
