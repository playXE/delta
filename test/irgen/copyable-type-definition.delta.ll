
%Foo = type { i32, i1 }

define void @_EN4main3Foo4initE1a3int1b4bool(%Foo* %this, i32 %a, i1 %b) {
  %a1 = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  store i32 %a, i32* %a1
  %b2 = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 1
  store i1 %b, i1* %b2
  %a3 = getelementptr inbounds %Foo, %Foo* %this, i32 0, i32 0
  %a3.load = load i32, i32* %a3
  %1 = add i32 %a3.load, 1
  store i32 %1, i32* %a3
  ret void
}

define i32 @main() {
  %f = alloca %Foo
  %bar = alloca i32
  %qux = alloca i1
  call void @_EN4main3Foo4initE1a3int1b4bool(%Foo* %f, i32 666, i1 true)
  %a = getelementptr inbounds %Foo, %Foo* %f, i32 0, i32 0
  %a.load = load i32, i32* %a
  %1 = add i32 %a.load, 1
  store i32 %1, i32* %a
  %a1 = getelementptr inbounds %Foo, %Foo* %f, i32 0, i32 0
  %a1.load = load i32, i32* %a1
  store i32 %a1.load, i32* %bar
  %b = getelementptr inbounds %Foo, %Foo* %f, i32 0, i32 1
  %b.load = load i1, i1* %b
  store i1 %b.load, i1* %qux
  ret i32 0
}
