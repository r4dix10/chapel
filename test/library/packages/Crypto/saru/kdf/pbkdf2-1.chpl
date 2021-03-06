proc main(){
  use Crypto;

  var hash = new owned Hash(Digest.SHA256);
  var k = new owned KDF(32, 1000, hash);
  var buf = new owned CryptoBuffer("random_salt");
  var key = k.passKDF("random_key", buf);
  writeln("Generated Key: ", key.toHex());
}
