
%X = type { i32 }
%Y = type { i32 }

define void @_EN4main1X4initE1i3int(%X* %this, i32 %i) {
  %i1 = getelementptr inbounds %X, %X* %this, i32 0, i32 0
  store i32 %i, i32* %i1
  ret void
}

define void @_EN4main1Y4initE1i3int(%Y* %this, i32 %i) {
  %i1 = getelementptr inbounds %Y, %Y* %this, i32 0, i32 0
  store i32 %i, i32* %i1
  ret void
}

define i1 @_EN4mainltE1aP1X1bP1X(%X* %a, %X* %b) {
  ret i1 true
}

define void @_EN4main2fxE1aP1X1bP1X(%X* %a, %X* %b) {
  %1 = call i1 @_EN4mainltE1aP1X1bP1X(%X* %a, %X* %b)
  %2 = call i1 @_EN4mainltE1aP1X1bP1X(%X* %a, %X* %b)
  ret void
}

define void @_EN4main2fyE1aP1Y1bP1Y(%Y* %a, %Y* %b) {
  %address = ptrtoint %Y* %a to i64
  %address1 = ptrtoint %Y* %b to i64
  %1 = icmp ult i64 %address, %address1
  ret void
}
