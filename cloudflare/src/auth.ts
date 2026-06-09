const PREFIX = "Bearer ";

export function extractBearerToken(req: Request): string | null {
  const header = req.headers.get("authorization");
  if (!header || !header.startsWith(PREFIX)) return null;
  return header.slice(PREFIX.length);
}

export function isAuthorized(req: Request, expectedToken: string | undefined): boolean {
  if (!expectedToken) return false;
  const token = extractBearerToken(req);
  return !!token && token === expectedToken;
}
