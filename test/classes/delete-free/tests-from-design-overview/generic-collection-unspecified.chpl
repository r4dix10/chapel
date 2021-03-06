class MyClass { var x:int; }

record Collection {
  var element;
}

proc Collection.addElement(arg: element.type) lifetime this < arg {
  element = arg;
}

proc test() {
  var c: Collection(owned MyClass);
  c.addElement( new owned MyClass() ); // transferred to element

  var global = new owned MyClass();

  var d: Collection(int);     d.addElement( 1 ); // OK
  var e: Collection(MyClass); e.addElement(global.borrow()); // OK
}

test();
