; Target datalayout and triple would go here
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Type definitions
%struct.Vec3 = type { double, double, double }
%struct.arche_array = type { i8*, i64, i64 }

declare i8* @malloc(i32)
declare void @free(i8*)
declare i8* @realloc(i8*, i64)
declare i32 @printf(i8*, ...)
declare void @abort()

@.arche_oob = private unnamed_addr constant [28 x i8] c"arche: index out of bounds\0A\00", align 1

declare i32 @write(i32, i8*, i32)
declare i32 @read(i32, i8*, i32)
declare i32 @open(i8*, i32)
declare i32 @close(i32)
declare i32 @exit(i32)
define void @print(%struct.arche_array* %arg0) {
entry:
  %v0 = getelementptr %struct.arche_array, %struct.arche_array* %arg0, i32 0, i32 1
  %v1 = load i64, i64* %v0
  %v2 = trunc i64 %v1 to i32
  %v3 = getelementptr %struct.arche_array, %struct.arche_array* %arg0, i32 0, i32 0
  %v4 = load i8*, i8** %v3
  %v5 = call i32 @write(i32 1, i8* %v4, i32 %v2)
  ret void
}

define i32 @main() {
entry:
  %v6 = alloca %struct.arche_array
  %v7 = getelementptr [14 x i8], [14 x i8]* @.arr0, i32 0, i32 0
  %v8 = getelementptr %struct.arche_array, %struct.arche_array* %v6, i32 0, i32 0
  store i8* %v7, i8** %v8
  %v9 = getelementptr %struct.arche_array, %struct.arche_array* %v6, i32 0, i32 1
  store i64 14, i64* %v9
  %v10 = getelementptr %struct.arche_array, %struct.arche_array* %v6, i32 0, i32 2
  store i64 14, i64* %v10
  %v11 = call i32 @print(%struct.arche_array* %v6)
  ret i32 0
}


; Global constants
@.arr0 = private constant [14 x i8] [i8 72, i8 101, i8 108, i8 108, i8 111, i8 44, i8 32, i8 87, i8 111, i8 114, i8 108, i8 100, i8 33, i8 10]


define i32 @print_double(double %val) {
entry:
  %fmt = getelementptr [3 x i8], [3 x i8]* @.print_fmt_double, i32 0, i32 0
  %res = call i32 (i8*, ...) @printf(i8* %fmt, double %val)
  ret i32 %res
}

@.print_fmt_double = private unnamed_addr constant [3 x i8] c"%g\00", align 1

attributes #0 = { "target-features"="+avx2,+avx" }
