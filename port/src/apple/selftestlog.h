#import <Foundation/Foundation.h>

// NSLog for self-test results: a line that reports a failure ("FAIL", "NOT",
// "TAKES", "NO LABEL") is counted, and the self-test exits with status 1 if any
// was. Each app's selftest.m has it.
void SelfTestLog(NSString* format, ...) NS_FORMAT_FUNCTION(1, 2);
