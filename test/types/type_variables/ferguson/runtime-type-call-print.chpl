proc getDom() {
  writeln("in getDom");
  return {1};
}


proc run() {
  type arrType = [getDom()] real;

  var a:arrType;

  // now a.domain should be {1}
  writeln(a.domain);
}

run();
