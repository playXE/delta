
define void @_EN4main1fE3fooP3int3barOP3int(i32* %foo, i32* %bar) {
  %1 = icmp eq i32* %foo, %bar
  br i1 %1, label %if.then, label %if.else

if.then:                                          ; preds = %0
  br label %if.end

if.else:                                          ; preds = %0
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  ret void
}
