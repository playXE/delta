
%"S<int>" = type { i32 }

define i32* @_EN4main1SI3intEixE1i3int(%"S<int>"* %this, i32 %i) {
  %t = getelementptr inbounds %"S<int>", %"S<int>"* %this, i32 0, i32 0
  ret i32* %t
}

define void @_EN4main1SI3intE4initE1t3int(%"S<int>"* %this, i32 %t) {
  %t1 = getelementptr inbounds %"S<int>", %"S<int>"* %this, i32 0, i32 0
  store i32 %t, i32* %t1
  ret void
}

define void @_EN4main1fE1sP1SI3intE(%"S<int>"* %s) {
  %1 = call i32* @_EN4main1SI3intEixE1i3int(%"S<int>"* %s, i32 0)
  %.load = load i32, i32* %1
  %2 = add i32 %.load, 1
  store i32 %2, i32* %1
  ret void
}
