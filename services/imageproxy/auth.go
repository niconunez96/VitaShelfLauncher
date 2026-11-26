package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	authMessagePrefix    = "vitashelf-image-auth-v1\n"
	challengeTTL         = 60 * time.Second
	sessionTTL           = 3600 * time.Second
	authTokenBytes       = 32
	maxCredentialBytes   = 128
	maxSessionBodyBytes  = 8192
	stateCleanupInterval = 30 * time.Second
)

var (
	errMalformedSession = errors.New("malformed session request")
	errInvalidChallenge = errors.New("invalid or expired challenge")
	errInvalidProof     = errors.New("invalid proof")
	errUnknownKeyID     = errors.New("unknown key id")
)

type authSession struct {
	installationID string
	expiresAt      time.Time
}

type sessionRequest struct {
	KeyID          string `json:"key_id"`
	InstallationID string `json:"installation_id"`
	Challenge      string `json:"challenge"`
	Proof          string `json:"proof"`
}

type authService struct {
	mu          sync.Mutex
	key         []byte
	keyID       string
	now         func() time.Time
	random      io.Reader
	logger      *log.Logger
	challenges  map[string]time.Time
	sessions    map[[sha256.Size]byte]authSession
	lastCleanup time.Time
}

// resolveAuthMode rejects partial credentials so a deployment cannot silently
// fall back to unauthenticated behavior while believing authentication is on.
func resolveAuthMode(key, keyID string) (bool, error) {
	switch {
	case key != "" && keyID != "":
		return true, nil
	case key == "" && keyID == "":
		return false, nil
	default:
		return false, errors.New("IMAGE_PROXY_KEY and IMAGE_PROXY_KEY_ID must both be set for public mode or both be empty for local mode")
	}
}

func newAuthService(key, keyID string, clock func() time.Time, random io.Reader, logger *log.Logger) *authService {
	if clock == nil {
		clock = time.Now
	}
	if random == nil {
		random = rand.Reader
	}
	if logger == nil {
		logger = log.Default()
	}
	return &authService{
		key:         []byte(key),
		keyID:       keyID,
		now:         clock,
		random:      random,
		logger:      logger,
		challenges:  make(map[string]time.Time),
		sessions:    make(map[[sha256.Size]byte]authSession),
		lastCleanup: clock(),
	}
}

func (service *authService) issueChallenge() (string, error) {
	raw, err := randomBytes(service.random, authTokenBytes)
	if err != nil {
		return "", err
	}
	value := base64.RawURLEncoding.EncodeToString(raw)
	now := service.now()

	service.mu.Lock()
	defer service.mu.Unlock()
	service.cleanupLocked(now)
	service.challenges[value] = now
	return value, nil
}

// createSession consumes the challenge before any other check: every
// verification attempt burns it, including attempts with a wrong key ID or proof.
func (service *authService) createSession(request sessionRequest) (string, error) {
	now := service.now()

	service.mu.Lock()
	issued, present := service.challenges[request.Challenge]
	delete(service.challenges, request.Challenge)
	service.cleanupLocked(now)
	service.mu.Unlock()

	if !present || now.Before(issued) || !now.Before(issued.Add(challengeTTL)) {
		return "", errInvalidChallenge
	}
	if subtle.ConstantTimeCompare([]byte(request.KeyID), []byte(service.keyID)) != 1 {
		return "", errUnknownKeyID
	}
	proof, err := base64.RawURLEncoding.Strict().DecodeString(request.Proof)
	if err != nil || len(proof) != sha256.Size {
		return "", errInvalidProof
	}
	message := authMessagePrefix + request.KeyID + "\n" + request.InstallationID + "\n" + request.Challenge + "\n"
	mac := hmac.New(sha256.New, service.key)
	_, _ = mac.Write([]byte(message))
	if subtle.ConstantTimeCompare(proof, mac.Sum(nil)) != 1 {
		return "", errInvalidProof
	}

	raw, err := randomBytes(service.random, authTokenBytes)
	if err != nil {
		return "", err
	}
	token := base64.RawURLEncoding.EncodeToString(raw)
	digest := sha256.Sum256([]byte(token))

	service.mu.Lock()
	defer service.mu.Unlock()
	service.cleanupLocked(now)
	service.sessions[digest] = authSession{
		installationID: request.InstallationID,
		expiresAt:      now.Add(sessionTTL),
	}
	return token, nil
}

// authenticate never records or logs the presented token; the in-memory index
// is keyed by its SHA-256 digest.
func (service *authService) authenticate(header string) (string, bool) {
	fields := strings.Fields(header)
	if len(fields) != 2 || !strings.EqualFold(fields[0], "Bearer") {
		return "", false
	}
	digest := sha256.Sum256([]byte(fields[1]))
	now := service.now()

	service.mu.Lock()
	defer service.mu.Unlock()
	session, present := service.sessions[digest]
	if !present || !now.Before(session.expiresAt) {
		delete(service.sessions, digest)
		return "", false
	}
	return session.installationID, true
}

func (service *authService) cleanupLocked(now time.Time) {
	if !now.Before(service.lastCleanup) && now.Sub(service.lastCleanup) < stateCleanupInterval {
		return
	}
	for challenge, issued := range service.challenges {
		if !now.Before(issued.Add(challengeTTL)) {
			delete(service.challenges, challenge)
		}
	}
	for digest, session := range service.sessions {
		if !now.Before(session.expiresAt) {
			delete(service.sessions, digest)
		}
	}
	service.lastCleanup = now
}

func randomBytes(source io.Reader, count int) ([]byte, error) {
	raw := make([]byte, count)
	if _, err := io.ReadFull(source, raw); err != nil {
		return nil, err
	}
	return raw, nil
}

func (service *handler) handleChallenge(writer http.ResponseWriter, request *http.Request) {
	if allowed, retryAfter := service.limiter.allowAuth(clientIP(request)); !allowed {
		writeRateLimit(writer, retryAfter)
		return
	}
	challenge, err := service.auth.issueChallenge()
	if err != nil {
		service.logger.Printf("issue challenge: %v", err)
		http.Error(writer, "could not issue challenge", http.StatusInternalServerError)
		return
	}
	writer.Header().Set("Content-Type", "application/json")
	writer.Header().Set("Cache-Control", "no-store")
	if err := json.NewEncoder(writer).Encode(struct {
		Challenge string `json:"challenge"`
		ExpiresIn int    `json:"expires_in"`
	}{Challenge: challenge, ExpiresIn: int(challengeTTL / time.Second)}); err != nil {
		service.logger.Printf("write challenge: %v", err)
	}
}

func (service *handler) handleSession(writer http.ResponseWriter, request *http.Request) {
	if allowed, retryAfter := service.limiter.allowAuth(clientIP(request)); !allowed {
		writeRateLimit(writer, retryAfter)
		return
	}

	var payload sessionRequest
	decoder := json.NewDecoder(io.LimitReader(request.Body, maxSessionBodyBytes))
	if err := decoder.Decode(&payload); err != nil {
		writeAuthError(writer, errMalformedSession)
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeAuthError(writer, errMalformedSession)
		return
	}
	if !validAuthField(payload.KeyID) || !validAuthField(payload.InstallationID) || !validAuthField(payload.Challenge) || payload.Proof == "" {
		writeAuthError(writer, errMalformedSession)
		return
	}

	token, err := service.auth.createSession(payload)
	if err != nil {
		writeAuthError(writer, err)
		return
	}
	writer.Header().Set("Content-Type", "application/json")
	writer.Header().Set("Cache-Control", "no-store")
	if err := json.NewEncoder(writer).Encode(struct {
		Token     string `json:"token"`
		ExpiresIn int    `json:"expires_in"`
	}{Token: token, ExpiresIn: int(sessionTTL / time.Second)}); err != nil {
		service.logger.Printf("write session: %v", err)
	}
}

func validAuthField(value string) bool {
	if len(value) == 0 || len(value) > maxCredentialBytes {
		return false
	}
	for index := 0; index < len(value); index++ {
		char := value[index]
		alphaNumeric := char >= 'a' && char <= 'z' || char >= 'A' && char <= 'Z' || char >= '0' && char <= '9'
		if !alphaNumeric && char != '.' && char != '_' && char != '-' {
			return false
		}
	}
	return true
}

func writeAuthError(writer http.ResponseWriter, err error) {
	status, message := http.StatusUnauthorized, "invalid credentials"
	switch {
	case errors.Is(err, errMalformedSession):
		status, message = http.StatusBadRequest, "malformed session request"
	case errors.Is(err, errInvalidChallenge):
		message = "invalid or expired challenge"
	case errors.Is(err, errInvalidProof):
		message = "invalid proof"
	case errors.Is(err, errUnknownKeyID):
		message = "unknown key id"
	default:
		status, message = http.StatusInternalServerError, "could not create session"
	}
	http.Error(writer, message, status)
}
